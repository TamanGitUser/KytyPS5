#include "cheatFile.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

extern "C" {
#include <libavutil/aes.h>
#include <libavutil/mem.h>
}

namespace Cheats {

namespace {

QString Tr(const char* text) {
	return QCoreApplication::translate("Cheats", text);
}

QString ValidateDocument(const QJsonDocument& document) {
	if (!document.isObject()) {
		return Tr("Invalid cheat JSON.");
	}
	const auto mods = document.object().value(QStringLiteral("mods"));
	if (!mods.isArray()) {
		return Tr("Cheat JSON has no \"mods\" array.");
	}
	for (const auto& value: mods.toArray()) {
		const auto mod     = value.toObject();
		const auto name    = mod.value(QStringLiteral("name"));
		const auto enabled = mod.value(QStringLiteral("enabled"));
		if (!value.isObject() || !name.isString() || name.toString().isEmpty() ||
		    (!enabled.isUndefined() && !enabled.isBool())) {
			return Tr("Invalid cheat entry.");
		}
	}
	return {};
}

// MC4 is base64 of AES-256-CBC with a fixed key and IV. The XML reader stops at the root end tag,
// before the padding.
void Decrypt(QByteArray* data) {
	static const uint8_t key[] = "304c6528f659c766110239a51cl5dd9c";
	uint8_t              iv[]  = "u@}kzW2u[u(8DWar";
	*data                      = QByteArray::fromBase64(*data);
	auto* aes                  = av_aes_alloc();
	auto* blocks               = reinterpret_cast<uint8_t*>(data->data());
	av_aes_init(aes, key, 256, 1);
	av_aes_crypt(aes, blocks, blocks, static_cast<int>(data->size() / 16), iv, 1);
	av_free(aes);
}

} // namespace

QString ConvertTrainer(QByteArray* data, bool encrypted) {
	if (encrypted) {
		Decrypt(data);
	}
	QXmlStreamReader xml(*data);
	if (!xml.readNextStartElement() || xml.name() != u"Trainer") {
		return Tr("Invalid cheat XML.");
	}
	const auto trainer = xml.attributes();
	QJsonArray mods;
	while (xml.readNextStartElement()) {
		if (xml.name() != u"Cheat") {
			xml.skipCurrentElement();
			continue;
		}
		QJsonObject mod {{QStringLiteral("name"), xml.attributes().value(u"Text").toString()}};
		QJsonArray  memory;
		while (xml.readNextStartElement()) {
			QJsonObject write;
			while (xml.readNextStartElement()) {
				const auto field = xml.name().toString();
				auto       value = xml.readElementText().trimmed();
				if (field == u"Offset") {
					write.insert(QStringLiteral("offset"), value);
				} else if (field == u"ValueOn") {
					write.insert(QStringLiteral("on"), value.remove(QLatin1Char('-')));
				} else if (field == u"ValueOff") {
					write.insert(QStringLiteral("off"), value.remove(QLatin1Char('-')));
				} else if (field == u"Section" && value != u"0") {
					// Section-relative and absolute writes are rejected on import.
					write.insert(QStringLiteral("section"), value);
				} else if (field == u"Absolute") {
					write.insert(QStringLiteral("absolute"), true);
				}
			}
			memory.append(write);
		}
		mod.insert(QStringLiteral("memory"), memory);
		mods.append(mod);
	}
	if (xml.hasError()) {
		return Tr("Invalid cheat XML.");
	}
	const QJsonObject root {
	    {QStringLiteral("name"), trainer.value(u"Game").toString()},
	    {QStringLiteral("id"), trainer.value(u"Cusa").toString()},
	    {QStringLiteral("version"), trainer.value(u"Version").toString()},
	    {QStringLiteral("process"), trainer.value(u"Process").toString()},
	    {QStringLiteral("credits"), QJsonArray {trainer.value(u"Moder").toString()}},
	    {QStringLiteral("mods"), mods}};
	*data = QJsonDocument(root).toJson();
	return {};
}

bool IsSupportedTitleId(const QString& title_id) {
	static const QRegularExpression pattern(QStringLiteral("\\APPSA[0-9]{5}\\z"));
	return pattern.match(title_id.trimmed().toUpper()).hasMatch();
}

QString PlanPath(const QString& title_id) {
	if (!IsSupportedTitleId(title_id)) {
		return {};
	}
	return QDir(QCoreApplication::applicationDirPath())
	    .filePath(QStringLiteral("_Patches/%1.json").arg(title_id.trimmed().toUpper()));
}

QString BuildLaunchPlan(const QString& title_id, const QString& version, const QString& process,
                        const QString& executable_path, int render_preset, QString* error) {
	if (error != nullptr) {
		error->clear();
	}
	const auto original_plan = PlanPath(title_id);
	if (render_preset == 0) {
		return original_plan;
	}
	const auto fail = [error](const QString& message) {
		if (error != nullptr) {
			*error = message;
		}
		return QString {};
	};
	// The instruction offsets below were verified for this one executable revision only.
	if (title_id.compare(QStringLiteral("PPSA01341"), Qt::CaseInsensitive) != 0 ||
	    version != QStringLiteral("01.007.000") ||
	    QFileInfo(process).fileName().compare(QStringLiteral("eboot.bin"), Qt::CaseInsensitive) != 0 ||
	    render_preset < 1 || render_preset > 4) {
		return fail(Tr("The selected render profile is only supported by Demon's Souls PPSA01341 version 01.007.000."));
	}

	// Refuse to apply a build-specific patch to a different eboot, even if title and app version match.
	QFile executable(executable_path);
	if (!executable.open(QIODevice::ReadOnly)) {
		return fail(Tr("Could not verify the Demon's Souls executable for the render profile."));
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	while (!executable.atEnd()) {
		const auto chunk = executable.read(1024 * 1024);
		if (chunk.isEmpty() && executable.error() != QFile::NoError) {
			return fail(Tr("Could not read the Demon's Souls executable for render-profile verification."));
		}
		if (!chunk.isEmpty()) {
			hash.addData(chunk);
		}
	}
	const auto executable_hash = QString::fromLatin1(hash.result().toHex());
	constexpr auto expected_hash = "d499226cdc4b074eb8ec72ac03ad469bb80f48e580164c08157e6fd0c608c2c5";
	if (executable_hash != QLatin1String(expected_hash)) {
		return fail(Tr("This render profile requires the verified Demon's Souls 01.007.000 eboot (SHA-256 mismatch). No game patch was generated."));
	}

	QJsonObject root;
	QJsonArray mods;
	if (QFileInfo::exists(original_plan)) {
		QFile user_file(original_plan);
		if (!user_file.open(QIODevice::ReadOnly)) {
			return fail(Tr("Could not read your existing game-patch file."));
		}
		QJsonDocument document;
		const auto parse_error = ParseDocument(user_file.readAll(), &document);
		if (!parse_error.isEmpty()) {
			return fail(Tr("Your existing game-patch file could not be merged: %1").arg(parse_error));
		}
		root = document.object();
		if (root.value(QStringLiteral("id")).toString().compare(title_id, Qt::CaseInsensitive) != 0 ||
		    root.value(QStringLiteral("version")).toString() != version ||
		    root.value(QStringLiteral("process")).toString().compare(QFileInfo(process).fileName(),
		                                                            Qt::CaseInsensitive) != 0) {
			return fail(Tr("Your existing game-patch file targets a different game version or executable; it was left unchanged."));
		}
		mods = root.value(QStringLiteral("mods")).toArray();
	} else {
		root.insert(QStringLiteral("name"), QStringLiteral("Demon's Souls"));
		root.insert(QStringLiteral("id"), title_id);
		root.insert(QStringLiteral("version"), version);
		root.insert(QStringLiteral("process"), QFileInfo(process).fileName());
	}

	// Remove only a previous generated GTX profile. All user-authored mods remain intact.
	const auto generated_prefix = QStringLiteral("GTX (MAXWELL) DeS render resolution: ");
	QJsonArray preserved_mods;
	for (const auto& value: mods) {
		const auto mod = value.toObject();
		if (!mod.value(QStringLiteral("name")).toString().startsWith(generated_prefix)) {
			preserved_mods.append(value);
		}
	}
	mods = preserved_mods;

	struct RenderPreset {
		const char* name;
		const char* render_pair;
	};
	static constexpr RenderPreset presets[] = {
	    {"", ""},
	    {"Ultra Quality 2944x1656", "800b000078060000"},
	    {"Quality 2560x1440", "000a0000a0050000"},
	    {"Balanced 2304x1296", "0009000010050000"},
	    {"Performance 1920x1080", "8007000038040000"},
	};
	const auto& selected = presets[render_preset];
	const auto write = [](const char* offset, const char* off, const char* on) {
		return QJsonObject {{QStringLiteral("offset"), QString::fromLatin1(offset)},
		                     {QStringLiteral("off"), QString::fromLatin1(off)},
		                     {QStringLiteral("on"), QString::fromLatin1(on)}};
	};
	QJsonArray memory;
	// The two control-flow patches and resolution pair are tied to the exact eboot SHA above.
	memory.append(write("0x90a66d", "c5fb1005bba1be01eb30", "c5fb100dbba1be01eb08"));
	memory.append(write("0x90b1d2", "c5fb10055696be01eb30", "c5fb100d5696be01eb08"));
	memory.append(write("0x24f9550", "000a0000a0050000", selected.render_pair));
	const QJsonObject generated_mod {
	    {QStringLiteral("name"), generated_prefix + QLatin1String(selected.name)},
	    {QStringLiteral("enabled"), true},
	    {QStringLiteral("memory"), memory}};
	mods.append(generated_mod);
	root.insert(QStringLiteral("mods"), mods);

	const auto launch_dir = QDir(QCoreApplication::applicationDirPath())
	                            .filePath(QStringLiteral("_Patches/_runtime"));
	if (!QDir().mkpath(launch_dir)) {
		return fail(Tr("Could not create the temporary render-profile patch directory."));
	}
	const auto output_path = QDir(launch_dir).filePath(
	    QStringLiteral("PPSA01341-01.007.000-render-%1.json").arg(render_preset));
	QSaveFile output(output_path);
	const auto data = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() ||
	    !output.commit()) {
		return fail(Tr("Could not write the launch-only render-profile patch: %1").arg(output.errorString()));
	}
	return output_path;
}

QString ParseDocument(const QByteArray& data, QJsonDocument* document) {
	const auto parsed = QJsonDocument::fromJson(data);
	const auto error  = ValidateDocument(parsed);
	*document         = error.isEmpty() ? parsed : QJsonDocument();
	return error;
}

QString ValidateImport(const QJsonDocument& document, const QString& title_id,
                       const QString& version, const QString& process) {
	const auto parse_error = ValidateDocument(document);
	if (!parse_error.isEmpty()) {
		return parse_error;
	}
	const auto root = document.object();
	if (!IsSupportedTitleId(title_id) ||
	    root.value(QStringLiteral("id"))
	            .toString()
	            .compare(title_id.trimmed(), Qt::CaseInsensitive) != 0) {
		return Tr("The cheat file does not match this game's title ID.");
	}
	if (version.isEmpty() || root.value(QStringLiteral("version")).toString() != version) {
		return Tr("The cheat file does not match this game's version (%1).").arg(version);
	}
	if (process.isEmpty() ||
	    root.value(QStringLiteral("process")).toString().compare(process, Qt::CaseInsensitive) !=
	        0) {
		return Tr("The cheat file does not match this game's executable (%1).").arg(process);
	}
	static const QRegularExpression offset_pattern(
	    QStringLiteral("\\A(?:0[xX])?[0-9a-fA-F]{1,16}\\z"));
	static const QRegularExpression bytes_pattern(QStringLiteral("\\A(?:[0-9a-fA-F]{2})+\\z"));
	const auto                      mods = root.value(QStringLiteral("mods")).toArray();
	if (mods.isEmpty()) {
		return Tr("The cheat file contains no cheats.");
	}
	if (root.contains(QStringLiteral("master"))) {
		return Tr("This JSON uses master codes, which are not supported yet.");
	}
	for (const auto& value: mods) {
		const auto mod    = value.toObject();
		const auto memory = mod.value(QStringLiteral("memory"));
		if (mod.contains(QStringLiteral("module_name"))) {
			return Tr("This JSON uses module-specific cheats, which are not supported yet.");
		}
		if (!memory.isArray() || memory.toArray().isEmpty()) {
			return Tr("Unsupported memory entries in cheat \"%1\".")
			    .arg(mod.value(QStringLiteral("name")).toString());
		}
		for (const auto& entry: memory.toArray()) {
			const auto write    = entry.toObject();
			const auto offset   = write.value(QStringLiteral("offset")).toString();
			const auto off      = write.value(QStringLiteral("off")).toString();
			const auto on       = write.value(QStringLiteral("on")).toString();
			const auto absolute = write.value(QStringLiteral("absolute"));
			const auto expected = write.value(QStringLiteral("expected"));
			if ((!absolute.isUndefined() && (!absolute.isBool() || absolute.toBool())) ||
			    (!expected.isUndefined() &&
			     (!expected.isString() ||
			      expected.toString().compare(off, Qt::CaseInsensitive) != 0)) ||
			    write.contains(QStringLiteral("section")) ||
			    write.contains(QStringLiteral("sectionName")) ||
			    write.contains(QStringLiteral("sectionProtection")) ||
			    write.contains(QStringLiteral("logicalOffset")) ||
			    write.contains(QStringLiteral("sectionOffset"))) {
				return Tr(
				    "This cheat file uses memory addressing features that are not supported yet.");
			}
			if (!entry.isObject() || !offset_pattern.match(offset).hasMatch() ||
			    !bytes_pattern.match(off).hasMatch() || !bytes_pattern.match(on).hasMatch() ||
			    off.size() != on.size()) {
				return Tr("Unsupported memory entries in cheat \"%1\".")
				    .arg(mod.value(QStringLiteral("name")).toString());
			}
		}
	}
	return {};
}

QJsonDocument WithSelection(const QJsonDocument& document, const QList<bool>& enabled) {
	auto root = document.object();
	auto mods = root.value(QStringLiteral("mods")).toArray();
	if (mods.size() != enabled.size()) {
		return {};
	}
	for (qsizetype index = 0; index < mods.size(); ++index) {
		auto mod = mods[index].toObject();
		mod.insert(QStringLiteral("enabled"), enabled[index]);
		mods[index] = mod;
	}
	root.insert(QStringLiteral("mods"), mods);
	return QJsonDocument(root);
}

QString SaveDocument(const QString& path, const QJsonDocument& document,
                     const QByteArray& expected_data) {
	if (path.isEmpty() || !document.isObject()) {
		return Tr("Invalid cheat file.");
	}
	QFile input(path);
	if (input.exists()) {
		if (!input.open(QIODevice::ReadOnly)) {
			return Tr("Could not read cheat file: %1").arg(input.errorString());
		}
		const auto current = input.readAll();
		if (input.error() != QFile::NoError) {
			return Tr("Could not read cheat file: %1").arg(input.errorString());
		}
		if (current != expected_data) {
			return Tr("Cheat file changed; reload the dialog.");
		}
	} else if (!expected_data.isEmpty()) {
		return Tr("Cheat file changed; reload the dialog.");
	}
	input.close();
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return Tr("Could not create the local cheats directory.");
	}
	QSaveFile  output(path);
	const auto data = document.toJson();
	if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() ||
	    !output.commit()) {
		return Tr("Could not save cheat file: %1").arg(output.errorString());
	}
	return {};
}

} // namespace Cheats
