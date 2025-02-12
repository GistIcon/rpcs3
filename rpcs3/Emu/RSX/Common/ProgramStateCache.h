#pragma once

#include "Emu/RSX/RSXFragmentProgram.h"
#include "Emu/RSX/RSXVertexProgram.h"
#include "Emu/Memory/vm.h"


enum class SHADER_TYPE
{
	SHADER_TYPE_VERTEX,
	SHADER_TYPE_FRAGMENT
};

namespace program_hash_util
{
	// Based on
	// https://github.com/AlexAltea/nucleus/blob/master/nucleus/gpu/rsx_pgraph.cpp
	union qword
	{
		u64 dword[2];
		u32 word[4];
	};

	struct vertex_program_hash
	{
		size_t operator()(const std::vector<u32> &program) const;
	};

	struct vertex_program_compare
	{
		bool operator()(const std::vector<u32> &binary1, const std::vector<u32> &binary2) const;
	};

	struct fragment_program_utils
	{
		/**
		* returns true if the given source Operand is a constant
		*/
		static bool is_constant(u32 sourceOperand);

		static size_t get_fragment_program_ucode_size(void *ptr);
	};

	struct fragment_program_hash
	{
		size_t operator()(const void *program) const;
	};

	struct fragment_program_compare
	{
		bool operator()(const void *binary1, const void *binary2) const;
	};
}


/**
* Cache for program help structure (blob, string...)
* The class is responsible for creating the object so the state only has to call getGraphicPipelineState
* Template argument is a struct which has the following type declaration :
* - a typedef VertexProgramData to a type that encapsulate vertex program info. It should provide an Id member.
* - a typedef FragmentProgramData to a types that encapsulate fragment program info. It should provide an Id member and a fragment constant offset vector.
* - a typedef PipelineData encapsulating monolithic program.
* - a typedef PipelineProperties to a type that encapsulate various state info relevant to program compilation (alpha test, primitive type,...)
* - a	typedef ExtraData type that will be passed to the buildProgram function.
* It should also contains the following function member :
* - static void recompile_fragment_program(RSXFragmentProgram *RSXFP, FragmentProgramData& fragmentProgramData, size_t ID);
* - static void recompile_vertex_program(RSXVertexProgram *RSXVP, VertexProgramData& vertexProgramData, size_t ID);
* - static PipelineData build_program(VertexProgramData &vertexProgramData, FragmentProgramData &fragmentProgramData, const PipelineProperties &pipelineProperties, const ExtraData& extraData);
*/
template<typename backend_traits>
class program_state_cache
{
	using pipeline_storage_type = typename backend_traits::pipeline_storage_type;
	using pipeline_properties = typename backend_traits::pipeline_properties;
	using vertex_program_type = typename backend_traits::vertex_program_type;
	using fragment_program_type = typename backend_traits::fragment_program_type;

	using binary_to_vertex_program = std::unordered_map<std::vector<u32>, vertex_program_type, program_hash_util::vertex_program_hash, program_hash_util::vertex_program_compare> ;
	using binary_to_fragment_program = std::unordered_map<void *, fragment_program_type, program_hash_util::fragment_program_hash, program_hash_util::fragment_program_compare>;


	struct pipeline_key
	{
		u32 vertex_program_id;
		u32 fragment_program_id;
		pipeline_properties properties;
	};

	struct pipeline_key_hash
	{
		size_t operator()(const pipeline_key &key) const
		{
			size_t hashValue = 0;
			hashValue ^= std::hash<unsigned>()(key.vertex_program_id);
			hashValue ^= std::hash<unsigned>()(key.fragment_program_id);
			hashValue ^= std::hash<pipeline_properties>()(key.properties);
			return hashValue;
		}
	};

	struct pipeline_key_compare
	{
		bool operator()(const pipeline_key &key1, const pipeline_key &key2) const
		{
			return (key1.vertex_program_id == key2.vertex_program_id) && (key1.fragment_program_id == key2.fragment_program_id) && (key1.properties == key2.properties);
		}
	};

private:
	size_t m_next_id = 0;
	binary_to_vertex_program m_vertex_shader_cache;
	binary_to_fragment_program m_fragment_shader_cache;
	std::unordered_map <pipeline_key, pipeline_storage_type, pipeline_key_hash, pipeline_key_compare> m_storage;

	/// bool here to inform that the program was preexisting.
	std::tuple<const vertex_program_type&, bool> search_vertex_program(const RSXVertexProgram& rsx_vp)
	{
		const auto& I = m_vertex_shader_cache.find(rsx_vp.data);
		if (I != m_vertex_shader_cache.end())
		{
			return std::forward_as_tuple(I->second, true);
		}
		LOG_NOTICE(RSX, "VP not found in buffer!");
		vertex_program_type& new_shader = m_vertex_shader_cache[rsx_vp.data];
		backend_traits::recompile_vertex_program(rsx_vp, new_shader, m_next_id++);

		return std::forward_as_tuple(new_shader, false);
	}

	/// bool here to inform that the program was preexisting.
	std::tuple<const fragment_program_type&, bool> search_fragment_program(const RSXFragmentProgram& rsx_fp)
	{
		const auto& I = m_fragment_shader_cache.find(vm::base(rsx_fp.addr));
		if (I != m_fragment_shader_cache.end())
		{
			return std::forward_as_tuple(I->second, true);
		}
		LOG_NOTICE(RSX, "FP not found in buffer!");
		size_t fragment_program_size = program_hash_util::fragment_program_utils::get_fragment_program_ucode_size(vm::base(rsx_fp.addr));
		gsl::not_null<void*> fragment_program_ucode_copy = malloc(fragment_program_size);
		std::memcpy(fragment_program_ucode_copy, vm::base(rsx_fp.addr), fragment_program_size);
		fragment_program_type &new_shader = m_fragment_shader_cache[fragment_program_ucode_copy];
		backend_traits::recompile_fragment_program(rsx_fp, new_shader, m_next_id++);

		return std::forward_as_tuple(new_shader, false);
	}

public:
	program_state_cache() = default;
	~program_state_cache() = default;

	const vertex_program_type& get_transform_program(const RSXVertexProgram& rsx_vp) const
	{
		auto I = m_vertex_shader_cache.find(rsx_vp.data);
		if (I == m_vertex_shader_cache.end())
			return I->second;
		throw new EXCEPTION("Trying to get unknow transform program");
	}

	const fragment_program_type& get_shader_program(const RSXFragmentProgram& rsx_fp) const
	{
		auto I = m_fragment_shader_cache.find(vm::base(rsx_fp.addr));
		if (I != m_fragment_shader_cache.end())
			return I->second;
		throw new EXCEPTION("Trying to get unknow shader program");
	}

	template<typename... Args>
	pipeline_storage_type& getGraphicPipelineState(
		const RSXVertexProgram& vertexShader,
		const RSXFragmentProgram& fragmentShader,
		const pipeline_properties& pipelineProperties,
		Args&& ...args
		)
	{
		// TODO : use tie and implicit variable declaration syntax with c++17
		const auto &vp_search = search_vertex_program(vertexShader);
		const auto &fp_search = search_fragment_program(fragmentShader);
		const vertex_program_type &vertex_program = std::get<0>(vp_search);
		const fragment_program_type &fragment_program = std::get<0>(fp_search);
		bool already_existing_fragment_program = std::get<1>(fp_search);
		bool already_existing_vertex_program = std::get<1>(vp_search);

		pipeline_key key = { vertex_program.id, fragment_program.id, pipelineProperties };

		if (already_existing_fragment_program && already_existing_vertex_program)
		{
			const auto I = m_storage.find(key);
			if (I != m_storage.end())
				return I->second;
		}

		LOG_NOTICE(RSX, "Add program :");
		LOG_NOTICE(RSX, "*** vp id = %d", vertex_program.id);
		LOG_NOTICE(RSX, "*** fp id = %d", fragment_program.id);

		m_storage[key] = backend_traits::build_pipeline(vertex_program, fragment_program, pipelineProperties, std::forward<Args>(args)...);
		return m_storage[key];
	}

	size_t get_fragment_constants_buffer_size(const RSXFragmentProgram &fragmentShader) const
	{
		const auto I = m_fragment_shader_cache.find(vm::base(fragmentShader.addr));
		if (I != m_fragment_shader_cache.end())
			return I->second.FragmentConstantOffsetCache.size() * 4 * sizeof(float);
		LOG_ERROR(RSX, "Can't retrieve constant offset cache");
		return 0;
	}

	void fill_fragment_constans_buffer(gsl::span<f32, gsl::dynamic_range> dst_buffer, const RSXFragmentProgram &fragment_program) const
	{
		const auto I = m_fragment_shader_cache.find(vm::base(fragment_program.addr));
		if (I == m_fragment_shader_cache.end())
			return;
		__m128i mask = _mm_set_epi8(0xE, 0xF, 0xC, 0xD,
			0xA, 0xB, 0x8, 0x9,
			0x6, 0x7, 0x4, 0x5,
			0x2, 0x3, 0x0, 0x1);

		Expects(dst_buffer.size_bytes() >= gsl::narrow<int>(I->second.FragmentConstantOffsetCache.size()) * 16);

		size_t offset = 0;
		for (size_t offset_in_fragment_program : I->second.FragmentConstantOffsetCache)
		{
			void *data = vm::base(fragment_program.addr + (u32)offset_in_fragment_program);
			const __m128i &vector = _mm_loadu_si128((__m128i*)data);
			const __m128i &shuffled_vector = _mm_shuffle_epi8(vector, mask);
			_mm_stream_si128((__m128i*)dst_buffer.subspan(offset, 4).data(), shuffled_vector);
			offset += sizeof(f32);
		}
	}
};
