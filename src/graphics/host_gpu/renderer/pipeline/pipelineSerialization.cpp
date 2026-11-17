// GTX (Maxwell) fork changes: modified/added 2026-11-08. See FORK-NOTICE.md; retain upstream notices.
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <ranges>
#include <string>
#include <xxhash.h>
#include <zstd.h>


// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Increment StoreFormatVersion when record serialization changes. Increment
// ShaderCompilerCacheVersion when translated SPIR-V semantics or specialization rules change.
// Unrelated executable rebuilds must not invalidate the cumulative game cache.
constexpr uint32_t StoreFormatVersion = 2;
constexpr uint32_t ShaderCompilerCacheVersion = 1;

// The store is cumulative across compatible Kyty builds. Its compatibility key includes the
// game identity/version and every host capability that affects emitted SPIR-V. The Vulkan
// driver's binary pipeline cache has its own stricter vendor/device/driver/UUID signature.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	std::string title_id;
	std::string content_id;
	std::string app_version;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		return {};
	}
	(void)Loader::SystemContentParamSfoGetString("CONTENT_ID", &content_id);
	(void)Loader::SystemContentParamSfoGetString("APP_VER", &app_version);

	const auto title_hash =
	    XXH3_64bits(title_id.data(), title_id.size());
	const auto content_hash = content_id.empty()
	                              ? 0ull
	                              : XXH3_64bits(content_id.data(), content_id.size());
	const auto version_hash = app_version.empty()
	                              ? 0ull
	                              : XXH3_64bits(app_version.data(), app_version.size());
	Writer header;
	IoAll(header, StoreFormatVersion, ShaderCompilerCacheVersion, title_hash, content_hash,
	      version_hash, SpirvProfile(graphics),
	      graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

// Record fields, shared by writing and reading.
template <class S, class Key> void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <typename... Args>
void PrecompilerLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	Log::WriteToConsoleAndLog(message);
}

template <class S, class Module> void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V; stored programs have no function code.
	auto& [id, handle, info, specialization, function_code, vertex_formats, rect_parameters,
	       mesh_task] = stage;
	IoAll(io, id, info, specialization, function_code, vertex_formats, rect_parameters, mesh_task);
}

} // namespace

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path) {
	if (path.empty()) {
		PrecompilerLog("Game precompiler: disabled (shader cache is off or this is not a Release build)");
		return;
	}
	const auto header = StoreHeader(m_graphics);
	if (header.empty()) {
		PrecompilerLog("Game precompiler: unavailable (could not identify the running title)");
		return;
	}
	const auto           start = std::chrono::steady_clock::now();
	std::vector<uint8_t> bytes;
	if (Common::File file;
	    Common::File::IsFileExisting(path) && file.Open(path, Common::File::Mode::Read)) {
		bytes.resize(file.Size());
		uint32_t read = 0;
		file.Read(bytes.data(), static_cast<uint32_t>(bytes.size()), &read);
		bytes.resize(read);
	}
	// NVIDIA drivers can crash inside the SPIR-V compiler when several pipelines share
	// one VkPipelineCache during warm-up. Keep this path serial on NVIDIA; other vendors
	// retain the existing parallel warm-up behaviour.
	const bool   serial  = m_graphics.physical_device_properties.vendorID == 0x10DEu;
	const size_t threads = serial ? 1u : Common::ParallelThreads();
	auto         records = std::span<const uint8_t>(bytes);
	const bool reusable =
	    records.size() >= header.size() && std::ranges::equal(records.first(header.size()), header);
	if (!reusable && !bytes.empty()) {
		PrecompilerLog("Game precompiler: previous store is incompatible with this game/profile; starting a new cumulative store");
	}
	if (reusable) {
		records = records.subspan(header.size());
		const size_t record_bytes = records.size();
		const size_t report_step = std::max<size_t>(1, record_bytes / 10);
		size_t       next_report = report_step;
		size_t       loaded_records = 0;
		while (LoadRecord(records)) {
			++loaded_records;
			const size_t loaded_bytes = record_bytes - records.size();
			if (loaded_bytes >= next_report || records.empty()) {
				const auto percent = record_bytes == 0 ? 100u : static_cast<unsigned>(
				    (100.0 * static_cast<double>(loaded_bytes)) / static_cast<double>(record_bytes));
				Log::WriteToConsoleAndLog(fmt::format(
				    "Game precompiler: restored {} records, {}% ({:.1f}/{:.1f} MB)\n", loaded_records,
				    percent, static_cast<double>(loaded_bytes) / 1'000'000.0,
				    static_cast<double>(record_bytes) / 1'000'000.0));
				next_report = loaded_bytes + report_step;
			}
		}
		if (!records.empty()) {
			PrecompilerLog("Game precompiler: discarded {} trailing bytes after the last complete cache record",
			                 records.size());
		}
	} else if (bytes.empty()) {
		PrecompilerLog("Game precompiler: no compatible prior store; this session will begin learning the game");
	}
	// A damaged record ends the store; complete records before it stay.
	m_store.Open(path, header, reusable ? bytes.size() - records.size() : 0);

	const auto modules = MapModules();
	const size_t module_count = modules.size() - 1; // Entry zero represents an absent shader stage.
	const size_t graphics_jobs = m_graphics_pipelines.size();
	size_t compute_jobs = 0;
	for (const auto* module: modules | std::views::values) {
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			++compute_jobs;
		}
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Game precompiler: cache contains {} shader modules, {} graphics pipeline states, "
	    "{} compute pipeline recipes and {} tessellation layouts\n",
	    module_count, graphics_jobs, compute_jobs, m_tess_layouts.size()));

	std::vector<Common::UniqueFunction<void>> jobs;
	jobs.reserve(graphics_jobs + compute_jobs);
	for (auto& node: m_graphics_pipelines) {
		jobs.emplace_back([this, &modules, &node] {
			CreateGraphicsPipeline(node.second, node.first, modules);
		});
	}
	// A compute pipeline depends only on its module.
	for (const auto* module: modules | std::views::values) {
		if (module != nullptr && module->info.stage == ShaderType::Compute) {
			jobs.emplace_back([this, module, &pipeline = m_compute_pipelines[module->id]] {
				CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
			});
		}
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Game precompiler: compiling {} graphics + {} compute pipelines with {} worker(s)\n",
	    graphics_jobs, compute_jobs, threads));
	if (serial) {
		const size_t report_step = std::max<size_t>(1, (jobs.size() + 9) / 10);
		for (size_t i = 0; i < jobs.size(); ++i) {
			jobs[i]();
			const size_t completed = i + 1;
			if (completed % report_step == 0 || completed == jobs.size()) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "Game precompiler: built {} / {} cached pipelines ({}%)\n", completed,
				    jobs.size(), completed * 100 / jobs.size()));
			}
		}
	} else {
		Common::ParallelFor(jobs.size(), [&](size_t i) { jobs[i](); });
	}
	// A key with a stage missing from the store keeps no pipeline.
	std::erase_if(m_graphics_pipelines,
	              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	if (!m_graphics_pipelines.empty() || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	                         std::chrono::steady_clock::now() - start)
	                         .count();
	Log::WriteToConsoleAndLog(fmt::format(
	    "Game precompiler: ready — {} graphics and {} compute pipelines from {} modules in {} ms\n",
	    m_graphics_pipelines.size(), m_compute_pipelines.size(), module_count, elapsed));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t>& records) {
	Frame frame;
	if (records.size() < sizeof(frame)) {
		return false;
	}
	std::memcpy(&frame, records.data(), sizeof(frame));
	if (records.size() - sizeof(frame) < frame.compressed_size) {
		return false;
	}
	const auto compressed = records.subspan(sizeof(frame), frame.compressed_size);
	if (XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
	    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
		return false;
	}
	std::vector<uint8_t> raw(frame.raw_size);
	if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) != raw.size()) {
		return false;
	}
	Reader     reader {raw};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			bool                  uses_calls = false;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan, uses_calls);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			program->second.uses_calls = uses_calls;
			module.module = CompileSPV(spirv, m_graphics.device);
			program->second.modules.push_back(std::move(module));
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			// WarmUp creates the pipeline once every record is loaded.
			m_graphics_pipelines.try_emplace(key);
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	records = records.subspan(sizeof(frame) + compressed.size());
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoAll(record, program.uses_calls);
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
	++m_session_stored_modules;
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
	++m_session_stored_graphics_pipelines;
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	m_store.Append(std::move(record.bytes));
	++m_session_stored_tessellation_layouts;
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
