#include "graphics/shader/rectListShader.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "spirv/unified1/spirv.hpp11"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Spirv::Builder;

constexpr uint32_t SpirvVersion15 = 0x00010500u;

template <typename T>
constexpr uint32_t Word(T value) {
	return static_cast<uint32_t>(value);
}

struct Parameter {
	uint32_t input_location  = 0;
	uint32_t output_location = 0;
	bool     flat            = false;
};

std::vector<Parameter> GetParameters(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info) {
	if (pixel_info == nullptr) {
		return {};
	}
	EXIT_IF(pixel_info->input_num > ShaderVertexInputInfo::RES_MAX);
	EXIT_IF(pixel_info->stage.program == nullptr);

	std::vector<uint32_t> active_inputs;
	for (const auto& input: pixel_info->stage.program->info.inputs) {
		if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
			active_inputs.push_back(input.location);
		}
	}

	std::vector<Parameter> parameters;
	for (const auto input: active_inputs) {
		const auto input_location = ShaderPixelParameterMappedLocation(*pixel_info, input);
		if ((vertex_info.stage.program->param_export_mask & (1u << input_location)) != 0) {
			parameters.push_back({input_location,
			                      ShaderPixelParameterLocation(*pixel_info, active_inputs, input),
			                      ShaderPixelParameterIsFlat(*pixel_info, input)});
		}
	}
	return parameters;
}

class RectListEmitter {
public:
	RectListEmitter(const std::vector<Parameter>& parameters_, spv::ExecutionModel model)
	    : parameters(parameters_) {
		builder.AddMemoryModel(
		    {Word(spv::AddressingModel::Logical), Word(spv::MemoryModel::GLSL450)});

		void_type       = Type(spv::Op::OpTypeVoid);
		uint_type       = Type(spv::Op::OpTypeInt, 32u, 0u);
		int_type        = Type(spv::Op::OpTypeInt, 32u, 1u);
		float_type      = Type(spv::Op::OpTypeFloat, 32u);
		vec4_float_type = Type(spv::Op::OpTypeVector, float_type, 4u);
		function_type   = Type(spv::Op::OpTypeFunction, void_type);

		per_vertex_type = builder.DecoratedType(
		    Word(spv::Op::OpTypeStruct), {vec4_float_type},
		    {{Word(spv::Op::OpMemberDecorate),
		      {0u, Word(spv::Decoration::BuiltIn), Word(spv::BuiltIn::Position)}},
		     {Word(spv::Op::OpDecorate), {Word(spv::Decoration::Block)}}});

		ptr_input_vec4_float  = Pointer(spv::StorageClass::Input, vec4_float_type);
		ptr_output_vec4_float = Pointer(spv::StorageClass::Output, vec4_float_type);
		if (model == spv::ExecutionModel::TessellationControl) {
			bool_type        = Type(spv::Op::OpTypeBool);
			vec2_bool_type   = Type(spv::Op::OpTypeVector, bool_type, 2u);
			vec2_float_type  = Type(spv::Op::OpTypeVector, float_type, 2u);
			ptr_output_float = Pointer(spv::StorageClass::Output, float_type);
		} else {
			vec3_float_type = Type(spv::Op::OpTypeVector, float_type, 3u);
			ptr_input_float = Pointer(spv::StorageClass::Input, float_type);
		}
	}

	std::vector<uint32_t> EmitControl() {
		DefineEntry(spv::ExecutionModel::TessellationControl);
		const auto float_one = Constant(float_type, std::bit_cast<uint32_t>(1.0f));

		for (uint32_t i = 0; i < 4; i++) {
			Store(Access(ptr_output_float, tess_outer, Int(i)), float_one);
		}
		for (uint32_t i = 0; i < 2; i++) {
			Store(Access(ptr_output_float, tess_inner, Int(i)), float_one);
		}

		std::array<uint32_t, 3> positions {};
		for (uint32_t i = 0; i < positions.size(); i++) {
			positions[i] =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, Int(i), Int(0)));
		}

		std::array<uint32_t, 3> coordinate_equal {};
		for (uint32_t i = 0; i < coordinate_equal.size(); i++) {
			const auto left  = Result(spv::Op::OpVectorShuffle, vec2_float_type, positions[i],
			                          positions[i], 0u, 1u);
			const auto right = Result(spv::Op::OpVectorShuffle, vec2_float_type,
			                          positions[(i + 1u) % 3u], positions[(i + 1u) % 3u], 0u, 1u);
			coordinate_equal[i] = Result(spv::Op::OpFOrdEqual, vec2_bool_type, left, right);
		}

		std::array<uint32_t, 3> barycentric {};
		std::array<uint32_t, 3> edge_vertex {};
		const auto float_minus_one = Constant(float_type, std::bit_cast<uint32_t>(-1.0f));
		for (uint32_t i = 0; i < edge_vertex.size(); i++) {
			const auto previous = (i + 2u) % 3u;
			const auto xy       = Result(
			    spv::Op::OpLogicalAnd, bool_type,
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[i], 0u),
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[previous], 1u));
			const auto yx = Result(
			    spv::Op::OpLogicalAnd, bool_type,
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[i], 1u),
			    Result(spv::Op::OpCompositeExtract, bool_type, coordinate_equal[previous], 0u));
			edge_vertex[i] = Result(spv::Op::OpLogicalOr, bool_type, xy, yx);
			barycentric[i] =
			    Result(spv::Op::OpSelect, float_type, edge_vertex[i], float_minus_one, float_one);
		}

		auto vertex_index = Result(spv::Op::OpSelect, int_type, edge_vertex[2], Int(2), Int(0));
		vertex_index = Result(spv::Op::OpSelect, int_type, edge_vertex[1], Int(1), vertex_index);
		const auto invocation = Load(int_type, invocation_id);
		const auto is_fourth  = Result(spv::Op::OpIEqual, bool_type, invocation, Int(3));
		const auto index =
		    Result(spv::Op::OpSMod, int_type,
		           Result(spv::Op::OpIAdd, int_type, vertex_index, invocation), Int(3));

		const auto position3 = Interpolate(positions[0], positions[1], positions[2], barycentric);
		const auto position =
		    Result(spv::Op::OpSelect, vec4_float_type, is_fourth, position3,
		           Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0))));
		Store(Access(ptr_output_vec4_float, gl_out, invocation, Int(0)), position);

		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto input0 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(0)));
			if (parameters[i].flat) {
				Store(Access(ptr_output_vec4_float, outputs[i], invocation), input0);
				continue;
			}
			const auto input1 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(1)));
			const auto input2 =
			    Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], Int(2)));
			const auto input3 = Interpolate(input0, input1, input2, barycentric);
			const auto value =
			    Result(spv::Op::OpSelect, vec4_float_type, is_fourth, input3,
			           Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
			Store(Access(ptr_output_vec4_float, outputs[i], invocation), value);
		}

		Emit(spv::Op::OpReturn);
		Emit(spv::Op::OpFunctionEnd);
		return builder.Build();
	}

	std::vector<uint32_t> EmitEvaluation() {
		DefineEntry(spv::ExecutionModel::TessellationEvaluation);

		const auto x     = Load(float_type, Access(ptr_input_float, tess_coord, Int(0)));
		const auto y     = Load(float_type, Access(ptr_input_float, tess_coord, Int(1)));
		const auto index = Result(
		    spv::Op::OpIAdd, int_type,
		    Result(spv::Op::OpIMul, int_type, Result(spv::Op::OpConvertFToS, int_type, y), Int(2)),
		    Result(spv::Op::OpConvertFToS, int_type, x));

		const auto position =
		    Load(vec4_float_type, Access(ptr_input_vec4_float, gl_in, index, Int(0)));
		Store(Access(ptr_output_vec4_float, gl_out, Int(0)), position);
		for (uint32_t i = 0; i < parameters.size(); i++) {
			Store(outputs[i],
			      Load(vec4_float_type, Access(ptr_input_vec4_float, inputs[i], index)));
		}

		Emit(spv::Op::OpReturn);
		Emit(spv::Op::OpFunctionEnd);
		return builder.Build();
	}

private:
	template <typename... Args>
	uint32_t Type(spv::Op opcode, Args... operands) {
		return builder.Type(Word(opcode), {Word(operands)...});
	}

	uint32_t Constant(uint32_t type, uint32_t value) {
		return builder.Constant(Word(spv::Op::OpConstant), type, {value});
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return Type(spv::Op::OpTypePointer, storage, type);
	}

	uint32_t Array(uint32_t type, uint32_t size) {
		return Type(spv::Op::OpTypeArray, type, Uint(size));
	}

	template <typename... Args>
	uint32_t Result(spv::Op opcode, uint32_t type, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction({Word(opcode), type, id, Word(operands)...});
		return id;
	}

	template <typename... Args>
	uint32_t ResultWithoutType(spv::Op opcode, Args... operands) {
		const auto id = builder.AllocateId();
		builder.AddFunction({Word(opcode), id, Word(operands)...});
		return id;
	}

	template <typename... Args>
	void Emit(spv::Op opcode, Args... operands) {
		builder.AddFunction({Word(opcode), Word(operands)...});
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		return Result(spv::Op::OpAccessChain, pointer_type, base, Word(indices)...);
	}

	uint32_t Load(uint32_t type, uint32_t pointer) {
		return Result(spv::Op::OpLoad, type, pointer);
	}

	void Store(uint32_t pointer, uint32_t value) { Emit(spv::Op::OpStore, pointer, value); }

	uint32_t Int(uint32_t value) { return Constant(int_type, value); }

	uint32_t Uint(uint32_t value) { return Constant(uint_type, value); }

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), Word(storage));
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation({Word(spv::Op::OpDecorate), target, Word(decoration), value});
	}

	void DefineEntry(spv::ExecutionModel model) {
		builder.RequireCapability(Word(spv::Capability::Shader));
		builder.RequireCapability(Word(spv::Capability::Tessellation));
		main = Result(spv::Op::OpFunction, void_type, spv::FunctionControlMask::MaskNone,
		              function_type);
		if (model == spv::ExecutionModel::TessellationControl) {
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::OutputVertices), 4u});
		} else {
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::Quads)});
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::SpacingEqual)});
			builder.AddExecutionMode({main, Word(spv::ExecutionMode::VertexOrderCw)});
		}
		DefineInputs(model);
		DefineOutputs(model);
		builder.AddEntryPoint(Word(model), main, "main", interfaces);
		ResultWithoutType(spv::Op::OpLabel);
	}

	void DefineInputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModel::TessellationControl;
		if (tess_control) {
			invocation_id = AddInterface(spv::StorageClass::Input, int_type);
			Decorate(invocation_id, spv::Decoration::BuiltIn, Word(spv::BuiltIn::InvocationId));
		} else {
			tess_coord = AddInterface(spv::StorageClass::Input, vec3_float_type);
			Decorate(tess_coord, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessCoord));
		}
		gl_in =
		    AddInterface(spv::StorageClass::Input, Array(per_vertex_type, tess_control ? 3u : 4u));

		inputs.resize(parameters.size());
		std::array<uint32_t, ShaderVertexInputInfo::RES_MAX> locations {};
		for (uint32_t i = 0; i < parameters.size(); i++) {
			const auto location =
			    tess_control ? parameters[i].input_location : parameters[i].output_location;
			if (tess_control && locations[location] != 0) {
				inputs[i] = locations[location];
				continue;
			}
			inputs[i] = AddInterface(spv::StorageClass::Input,
			                         Array(vec4_float_type, tess_control ? 3u : 4u));
			Decorate(inputs[i], spv::Decoration::Location, location);
			locations[location] = inputs[i];
		}
	}

	void DefineOutputs(spv::ExecutionModel model) {
		const auto tess_control = model == spv::ExecutionModel::TessellationControl;
		if (tess_control) {
			gl_out     = AddInterface(spv::StorageClass::Output, Array(per_vertex_type, 4u));
			tess_inner = AddInterface(spv::StorageClass::Output, Array(float_type, 2u));
			Decorate(tess_inner, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessLevelInner));
			builder.AddAnnotation(
			    {Word(spv::Op::OpDecorate), tess_inner, Word(spv::Decoration::Patch)});
			tess_outer = AddInterface(spv::StorageClass::Output, Array(float_type, 4u));
			Decorate(tess_outer, spv::Decoration::BuiltIn, Word(spv::BuiltIn::TessLevelOuter));
			builder.AddAnnotation(
			    {Word(spv::Op::OpDecorate), tess_outer, Word(spv::Decoration::Patch)});
		} else {
			gl_out = AddInterface(spv::StorageClass::Output, per_vertex_type);
		}

		outputs.resize(parameters.size());
		for (uint32_t i = 0; i < parameters.size(); i++) {
			outputs[i] = AddInterface(spv::StorageClass::Output,
			                          tess_control ? Array(vec4_float_type, 4u) : vec4_float_type);
			Decorate(outputs[i], spv::Decoration::Location, parameters[i].output_location);
		}
	}

	uint32_t Interpolate(uint32_t v0, uint32_t v1, uint32_t v2,
	                     const std::array<uint32_t, 3>& barycentric) {
		const auto p0 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v0, barycentric[0]);
		const auto p1 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v1, barycentric[1]);
		const auto p2 = Result(spv::Op::OpVectorTimesScalar, vec4_float_type, v2, barycentric[2]);
		return Result(spv::Op::OpFAdd, vec4_float_type, p0,
		              Result(spv::Op::OpFAdd, vec4_float_type, p1, p2));
	}

	Builder                       builder {SpirvVersion15};
	const std::vector<Parameter>& parameters;
	std::vector<uint32_t>         interfaces;
	std::vector<uint32_t>         inputs;
	std::vector<uint32_t>         outputs;
	uint32_t                      main                  = 0;
	uint32_t                      void_type             = 0;
	uint32_t                      bool_type             = 0;
	uint32_t                      uint_type             = 0;
	uint32_t                      int_type              = 0;
	uint32_t                      float_type            = 0;
	uint32_t                      vec2_bool_type        = 0;
	uint32_t                      vec2_float_type       = 0;
	uint32_t                      vec3_float_type       = 0;
	uint32_t                      vec4_float_type       = 0;
	uint32_t                      function_type         = 0;
	uint32_t                      per_vertex_type       = 0;
	uint32_t                      ptr_input_float       = 0;
	uint32_t                      ptr_input_vec4_float  = 0;
	uint32_t                      ptr_output_float      = 0;
	uint32_t                      ptr_output_vec4_float = 0;
	uint32_t                      gl_in                 = 0;
	uint32_t                      gl_out                = 0;
	uint32_t                      tess_inner            = 0;
	uint32_t                      tess_outer            = 0;
	uint32_t                      tess_coord            = 0;
	uint32_t                      invocation_id         = 0;
};

} // namespace


namespace {

struct BarycentricSourceParameter {
	uint32_t location = 0;
};

struct BarycentricParameter {
	uint32_t source_location = 0;
	uint32_t pixel_location = 0;
	uint32_t software_location = 0;
	bool per_vertex = false;
	bool flat = false;
	bool no_perspective = false;
};

struct BarycentricPerVertexOutputs {
	bool point_size = false;
	uint32_t clip_distance_count = 0;
	uint32_t cull_distance_count = 0;
	bool layer = false;
	bool viewport_index = false;
};

class BarycentricGeometryEmitter {
public:
	BarycentricGeometryEmitter(const std::vector<BarycentricSourceParameter>& sources_,
	                           std::vector<BarycentricParameter>& parameters_,
	                           bool no_perspective_, uint32_t barycentric_location_,
	                           BarycentricPerVertexOutputs builtins_)
	    : sources(sources_), parameters(parameters_), no_perspective(no_perspective_),
	      barycentric_location(barycentric_location_), builtins(builtins_) {}

	std::vector<uint32_t> Emit() {
		builder.RequireCapability(Word(spv::Capability::Shader));
		builder.RequireCapability(Word(spv::Capability::Geometry));
		builder.AddMemoryModel(
		    {Word(spv::AddressingModel::Logical), Word(spv::MemoryModel::GLSL450)});
		void_type       = Type(spv::Op::OpTypeVoid);
		uint_type       = Type(spv::Op::OpTypeInt, 32u, 0u);
		int_type        = Type(spv::Op::OpTypeInt, 32u, 1u);
		float_type      = Type(spv::Op::OpTypeFloat, 32u);
		vec3_float_type = Type(spv::Op::OpTypeVector, float_type, 3u);
		vec4_float_type = Type(spv::Op::OpTypeVector, float_type, 4u);
		function_type   = Type(spv::Op::OpTypeFunction, void_type);

		DefinePerVertexBlock();
		ptr_input_vec4  = Pointer(spv::StorageClass::Input, vec4_float_type);
		ptr_output_vec4 = Pointer(spv::StorageClass::Output, vec4_float_type);
		ptr_input_float = Pointer(spv::StorageClass::Input, float_type);
		ptr_output_float = Pointer(spv::StorageClass::Output, float_type);
		ptr_input_int   = Pointer(spv::StorageClass::Input, uint_type);

		gl_in = AddInterface(spv::StorageClass::Input, Array(per_vertex_type, 3u));
		gl_out = AddInterface(spv::StorageClass::Output, per_vertex_type);
		DefineBuiltInInterfaces();

		DefineParameterInterfaces();
		DefineBarycentricInterface();

		main = builder.AllocateId();
		builder.AddExecutionMode({main, Word(spv::ExecutionMode::Triangles)});
		builder.AddExecutionMode({main, Word(spv::ExecutionMode::OutputTriangleStrip)});
		builder.AddExecutionMode({main, Word(spv::ExecutionMode::OutputVertices), 3u});
		builder.AddEntryPoint(Word(spv::ExecutionModel::Geometry), main, "main", interfaces);
		builder.AddName(main, "main");
		EmitFunction();
		return builder.Build();
	}

private:
	template <typename... Args>
	uint32_t Type(spv::Op opcode, Args... operands) {
		return builder.Type(Word(opcode), {Word(operands)...});
	}

	uint32_t Array(uint32_t element, uint32_t count) {
		return builder.Type(Word(spv::Op::OpTypeArray), {element, Uint(count)});
	}

	uint32_t Pointer(spv::StorageClass storage, uint32_t type) {
		return Type(spv::Op::OpTypePointer, storage, type);
	}

	uint32_t Uint(uint32_t value) {
		return builder.Constant(Word(spv::Op::OpConstant), uint_type, {value});
	}

	uint32_t Int(uint32_t value) {
		return builder.Constant(Word(spv::Op::OpConstant), int_type, {value});
	}

	uint32_t Float(float value) {
		return builder.Constant(Word(spv::Op::OpConstant), float_type,
		                        {std::bit_cast<uint32_t>(value)});
	}

	uint32_t AddInterface(spv::StorageClass storage, uint32_t type) {
		const auto variable = builder.DefineGlobalVariable(Pointer(storage, type), Word(storage));
		interfaces.push_back(variable);
		return variable;
	}

	void Decorate(uint32_t target, spv::Decoration decoration, uint32_t value) {
		builder.AddAnnotation({Word(spv::Op::OpDecorate), target, Word(decoration), value});
	}

	void DecorateFlag(uint32_t target, spv::Decoration decoration) {
		builder.AddAnnotation({Word(spv::Op::OpDecorate), target, Word(decoration)});
	}

	void DefinePerVertexBlock() {
		per_vertex_type = builder.DecoratedType(
		    Word(spv::Op::OpTypeStruct), {vec4_float_type},
		    {{Word(spv::Op::OpMemberDecorate),
		      {0u, Word(spv::Decoration::BuiltIn), Word(spv::BuiltIn::Position)}},
		     {Word(spv::Op::OpDecorate), {Word(spv::Decoration::Block)}}});
	}

	void DefineBuiltInInterfaces() {
		const auto define = [&](uint32_t& input, uint32_t& output, spv::BuiltIn builtin,
		                        uint32_t input_type, uint32_t output_type) {
			input = AddInterface(spv::StorageClass::Input, input_type);
			output = AddInterface(spv::StorageClass::Output, output_type);
			Decorate(input, spv::Decoration::BuiltIn, Word(builtin));
			Decorate(output, spv::Decoration::BuiltIn, Word(builtin));
		};
		if (builtins.point_size) {
			define(point_size_input, point_size_output, spv::BuiltIn::PointSize,
			       Array(float_type, 3u), float_type);
		}
		if (builtins.clip_distance_count != 0) {
			define(clip_distance_input, clip_distance_output, spv::BuiltIn::ClipDistance,
			       Array(Array(float_type, builtins.clip_distance_count), 3u),
			       Array(float_type, builtins.clip_distance_count));
			builder.RequireCapability(Word(spv::Capability::ClipDistance));
		}
		if (builtins.cull_distance_count != 0) {
			define(cull_distance_input, cull_distance_output, spv::BuiltIn::CullDistance,
			       Array(Array(float_type, builtins.cull_distance_count), 3u),
			       Array(float_type, builtins.cull_distance_count));
			builder.RequireCapability(Word(spv::Capability::CullDistance));
		}
		if (builtins.layer) {
			define(layer_input, layer_output, spv::BuiltIn::Layer,
			       Array(uint_type, 3u), uint_type);
			builder.RequireVersion(SpirvVersion15);
			builder.RequireCapability(Word(spv::Capability::ShaderLayer));
		}
		if (builtins.viewport_index) {
			define(viewport_index_input, viewport_index_output, spv::BuiltIn::ViewportIndex,
			       Array(uint_type, 3u), uint_type);
			builder.RequireVersion(SpirvVersion15);
			builder.RequireCapability(Word(spv::Capability::ShaderViewportIndex));
		}
	}

	void DefineParameterInterfaces() {
		const auto input_type = Array(vec4_float_type, 3u);
		for (auto& source: sources) {
			source_variables.push_back(AddInterface(spv::StorageClass::Input, input_type));
			Decorate(source_variables.back(), spv::Decoration::Location, source.location);
		}
		for (auto& parameter: parameters) {
			std::array<uint32_t, 3> variables {};
			const auto variable_count = parameter.per_vertex ? 3u : 1u;
			for (uint32_t vertex = 0; vertex < variable_count; vertex++) {
				variables[vertex] = AddInterface(spv::StorageClass::Output, vec4_float_type);
				Decorate(variables[vertex], spv::Decoration::Location,
				         parameter.software_location + vertex);
				if (parameter.per_vertex || parameter.flat) {
					DecorateFlag(variables[vertex], spv::Decoration::Flat);
				} else if (parameter.no_perspective) {
					DecorateFlag(variables[vertex], spv::Decoration::NoPerspective);
				}
			}
			parameter_variables.push_back(variables);
		}
	}

	void DefineBarycentricInterface() {
		barycentric_output = AddInterface(spv::StorageClass::Output, vec3_float_type);
		Decorate(barycentric_output, spv::Decoration::Location,
		         barycentric_location);
		if (no_perspective) {
			DecorateFlag(barycentric_output, spv::Decoration::NoPerspective);
		}
		const auto zero = Float(0.0f);
		const auto one  = Float(1.0f);
		barycentric_basis[0] = builder.Constant(
		    Word(spv::Op::OpConstantComposite), vec3_float_type, {one, zero, zero});
		barycentric_basis[1] = builder.Constant(
		    Word(spv::Op::OpConstantComposite), vec3_float_type, {zero, one, zero});
		barycentric_basis[2] = builder.Constant(
		    Word(spv::Op::OpConstantComposite), vec3_float_type, {zero, zero, one});
		zero_vec4 = builder.Constant(Word(spv::Op::OpConstantComposite), vec4_float_type,
		                             {zero, zero, zero, zero});
	}

	template <typename... Args>
	uint32_t Access(uint32_t pointer_type, uint32_t base, Args... indices) {
		const auto pointer = builder.AllocateId();
		builder.AddFunction({Word(spv::Op::OpAccessChain), pointer_type, pointer,
		                     base, Word(indices)...});
		return pointer;
	}

	uint32_t Load(uint32_t type, uint32_t pointer) {
		const auto value = builder.AllocateId();
		builder.AddFunction({Word(spv::Op::OpLoad), type, value, pointer});
		return value;
	}

	void Store(uint32_t pointer, uint32_t value) {
		builder.AddFunction({Word(spv::Op::OpStore), pointer, value});
	}

	uint32_t LoadPerVertexPosition(uint32_t vertex) {
		const auto pointer = Access(ptr_input_vec4, gl_in, Int(vertex), Int(position_member));
		return Load(vec4_float_type, pointer);
	}

	void StorePerVertexPosition(uint32_t value) {
		Store(Access(ptr_output_vec4, gl_out, Int(position_member)), value);
	}

	void CopyPerVertexFloat(uint32_t vertex, uint32_t input, uint32_t output,
	                         uint32_t component_count) {
		for (uint32_t component = 0; component < component_count; component++) {
			const auto value = Load(float_type, Access(ptr_input_float, input, Int(vertex), Int(component)));
			Store(Access(ptr_output_float, output, Int(component)), value);
		}
	}

	void CopyPerVertexScalar(uint32_t vertex) {
		const auto value = Load(float_type, Access(ptr_input_float, point_size_input, Int(vertex)));
		Store(point_size_output, value);
	}

	void CopyPerVertexInteger(uint32_t vertex, uint32_t input, uint32_t output) {
		const auto value = Load(uint_type, Access(ptr_input_int, input, Int(vertex)));
		Store(output, value);
	}

	uint32_t SourceVariable(uint32_t location) const {
		for (size_t i = 0; i < sources.size(); i++) {
			if (sources[i].location == location) return source_variables[i];
		}
		return 0;
	}

	void EmitFunction() {
		builder.AddFunction({Word(spv::Op::OpFunction), void_type, main,
		                     Word(spv::FunctionControlMask::MaskNone), function_type});
		const auto label = builder.AllocateId();
		builder.AddFunction({Word(spv::Op::OpLabel), label});
		for (uint32_t vertex = 0; vertex < 3; vertex++) {
			StorePerVertexPosition(LoadPerVertexPosition(vertex));
			if (builtins.point_size) CopyPerVertexScalar(vertex);
			if (builtins.clip_distance_count != 0) {
				CopyPerVertexFloat(vertex, clip_distance_input, clip_distance_output,
				                   builtins.clip_distance_count);
			}
			if (builtins.cull_distance_count != 0) {
				CopyPerVertexFloat(vertex, cull_distance_input, cull_distance_output,
				                   builtins.cull_distance_count);
			}
			if (builtins.layer) CopyPerVertexInteger(vertex, layer_input, layer_output);
			if (builtins.viewport_index) {
				CopyPerVertexInteger(vertex, viewport_index_input, viewport_index_output);
			}

			for (size_t p = 0; p < parameters.size(); p++) {
				const auto& parameter = parameters[p];
				const auto  source    = SourceVariable(parameter.source_location);
				if (parameter.per_vertex) {
					for (uint32_t source_vertex = 0; source_vertex < 3; source_vertex++) {
						const auto value = source != 0
						    ? Load(vec4_float_type, Access(ptr_input_vec4, source, Int(source_vertex)))
						    : zero_vec4;
						Store(parameter_variables[p][source_vertex], value);
					}
				} else {
					const auto value = source != 0
					    ? Load(vec4_float_type, Access(ptr_input_vec4, source, Int(vertex)))
					    : zero_vec4;
					Store(parameter_variables[p][0], value);
				}
			}
			Store(barycentric_output, barycentric_basis[vertex]);
			builder.AddFunction({Word(spv::Op::OpEmitVertex)});
		}
		builder.AddFunction({Word(spv::Op::OpEndPrimitive)});
		builder.AddFunction({Word(spv::Op::OpReturn)});
		builder.AddFunction({Word(spv::Op::OpFunctionEnd)});
	}

	Builder builder {SpirvVersion15};
	const std::vector<BarycentricSourceParameter>& sources;
	std::vector<BarycentricParameter>& parameters;
	bool no_perspective = false;
	uint32_t barycentric_location = 0;
	BarycentricPerVertexOutputs builtins;
	std::vector<uint32_t> interfaces;
	std::vector<uint32_t> source_variables;
	std::vector<std::array<uint32_t, 3>> parameter_variables;
	uint32_t main = 0, void_type = 0, uint_type = 0, int_type = 0, float_type = 0;
	uint32_t vec3_float_type = 0, vec4_float_type = 0, function_type = 0, per_vertex_type = 0;
	uint32_t gl_in = 0, gl_out = 0, barycentric_output = 0, zero_vec4 = 0;
	uint32_t position_member = 0;
	uint32_t ptr_input_vec4 = 0, ptr_output_vec4 = 0, ptr_input_float = 0, ptr_output_float = 0;
	uint32_t ptr_input_int = 0;
	uint32_t point_size_input = 0, point_size_output = 0;
	uint32_t clip_distance_input = 0, clip_distance_output = 0;
	uint32_t cull_distance_input = 0, cull_distance_output = 0;
	uint32_t layer_input = 0, layer_output = 0;
	uint32_t viewport_index_input = 0, viewport_index_output = 0;
	std::array<uint32_t, 3> barycentric_basis {};
};

} // namespace

std::vector<uint32_t> BuildFragmentBarycentricGeometryShader(
    const ShaderVertexInputInfo& vertex_info, const ShaderPixelInputInfo& pixel_info,
    bool rect_list) {
	if (!vertex_info.stage || !pixel_info.stage ||
	    vertex_info.stage.program->stage == ShaderType::Mesh) {
		return {};
	}
	const auto& vertex_outputs = vertex_info.stage.program->info.outputs;
	const auto& pixel_inputs  = pixel_info.stage.program->info.inputs;
	const auto barycentric = std::find_if(pixel_inputs.begin(), pixel_inputs.end(), [](const auto& input) {
		return input.kind == ShaderRecompiler::IR::StageInputKind::BaryCoordSmooth ||
		       input.kind == ShaderRecompiler::IR::StageInputKind::BaryCoordNoPerspective;
	});
	const bool has_per_vertex_parameters = std::any_of(pixel_inputs.begin(), pixel_inputs.end(), [](const auto& input) {
		return input.kind == ShaderRecompiler::IR::StageInputKind::Parameter && input.per_vertex;
	});
	if (barycentric == pixel_inputs.end() && !has_per_vertex_parameters) return {};

	std::vector<uint32_t> active_inputs;
	for (const auto& input: pixel_inputs) {
		if (input.kind == ShaderRecompiler::IR::StageInputKind::Parameter) {
			active_inputs.push_back(input.location);
		}
	}
	std::vector<BarycentricSourceParameter> sources;
	std::vector<BarycentricParameter> parameters;
	uint32_t software_location = 0;
	for (const auto& input: pixel_inputs) {
		if (input.kind != ShaderRecompiler::IR::StageInputKind::Parameter) continue;
		const auto vertex_source_location =
		    ShaderPixelParameterMappedLocation(pixel_info, input.location);
		const auto source_location =
		    rect_list ? ShaderPixelParameterLocation(pixel_info, active_inputs, input.location)
		              : vertex_source_location;
		const auto source_exists = std::any_of(vertex_outputs.begin(), vertex_outputs.end(),
		    [vertex_source_location](const auto& output) {
			    return output.kind == ShaderRecompiler::IR::StageOutputKind::Parameter &&
			           output.index == vertex_source_location;
		    });
		if (source_exists && std::none_of(sources.begin(), sources.end(), [source_location](const auto& source) {
			    return source.location == source_location;
		    })) {
			sources.push_back({source_location});
		}
		parameters.push_back({
		    .source_location = source_location,
		    .pixel_location = software_location,
		    .software_location = software_location,
		    .per_vertex = input.per_vertex,
		    .flat = ShaderPixelParameterIsFlat(pixel_info, input.location),
		    .no_perspective = pixel_info.ps_no_perspective,
		});
		software_location += input.per_vertex ? 3u : 1u;
	}

	BarycentricPerVertexOutputs builtins;
	if (!rect_list) for (const auto& output: vertex_outputs) {
		switch (output.kind) {
			case ShaderRecompiler::IR::StageOutputKind::PointSize:
				builtins.point_size = true;
				break;
			case ShaderRecompiler::IR::StageOutputKind::ClipDistance:
				builtins.clip_distance_count = std::max(builtins.clip_distance_count, output.index + 1u);
				break;
			case ShaderRecompiler::IR::StageOutputKind::CullDistance:
				builtins.cull_distance_count = std::max(builtins.cull_distance_count, output.index + 1u);
				break;
			case ShaderRecompiler::IR::StageOutputKind::Layer:
				builtins.layer = true;
				break;
			case ShaderRecompiler::IR::StageOutputKind::ViewportIndex:
				builtins.viewport_index = true;
				break;
			default: break;
		}
	}
	const bool no_perspective =
	    barycentric != pixel_inputs.end()
	        ? barycentric->kind == ShaderRecompiler::IR::StageInputKind::BaryCoordNoPerspective
	        : pixel_info.ps_no_perspective;
	BarycentricGeometryEmitter emitter(sources, parameters, no_perspective,
	                                      software_location, builtins);
	return emitter.Emit();
}

RectListShaders BuildRectListShaders(const ShaderVertexInputInfo& vertex_info,
                                     const ShaderPixelInputInfo*  pixel_info) {
	const auto      parameters = GetParameters(vertex_info, pixel_info);
	RectListEmitter control(parameters, spv::ExecutionModel::TessellationControl);
	RectListEmitter evaluation(parameters, spv::ExecutionModel::TessellationEvaluation);
	return {control.EmitControl(), evaluation.EmitEvaluation()};
}

} // namespace Libs::Graphics
