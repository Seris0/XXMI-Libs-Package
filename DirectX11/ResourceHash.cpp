#include "ResourceHash.h"

#include <INITGUID.h>
#include "log.h"
#include "util.h"
#include "globals.h"
#include "profiling.h"
#include "overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// DirectXTK headers fail to include their own pre-requisits. We just want
// GetSurfaceInfo from LoaderHelpers
#include "DirectXTK/Src/pch.h"
#include "DirectXTK/Src/PlatformHelpers.h"
#include "DirectXTK/Src/LoaderHelpers.h"

static UINT CompressedFormatBlockSize(DXGI_FORMAT Format);

namespace {
	static const UINT TEXTURE_PHASH_NORMALIZED_EDGE = 126;
	static const UINT TEXTURE_PHASH_DCT_EDGE = 32;
	static const UINT TEXTURE_PHASH_LOW_FREQ_EDGE = 8;
	static const UINT TEXTURE_PHASH_MAX_HAMMING_DISTANCE = 6;
	static const float TEXTURE_PHASH_PI = 3.14159265358979323846f;

	struct TexturePHashL2Key
	{
		uint64_t raw_hash;
		uint16_t aspect_bucket;
		uint8_t luma_bucket;

		bool operator==(const TexturePHashL2Key& other) const
		{
			return raw_hash == other.raw_hash
				&& aspect_bucket == other.aspect_bucket
				&& luma_bucket == other.luma_bucket;
		}
	};

	struct TexturePHashL2KeyHasher
	{
		size_t operator()(const TexturePHashL2Key& key) const
		{
			size_t seed = std::hash<uint64_t>()(key.raw_hash);
			seed ^= (size_t)key.aspect_bucket + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			seed ^= (size_t)key.luma_bucket + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			return seed;
		}
	};

	struct TexturePHashCanonicalEntry
	{
		uint64_t canonical_hash;
		uint16_t aspect_bucket;
		uint8_t luma_bucket;
	};

	struct PHashPixelSampleCache
	{
		int block_x;
		int block_y;
		float block_luma[16];
		bool valid;

		PHashPixelSampleCache() :
			block_x(-1),
			block_y(-1),
			valid(false)
		{}
	};

	struct TexturePHashInfo
	{
		uint64_t raw_hash;
		uint64_t canonical_hash;
		uint16_t aspect_bucket;
		uint8_t luma_bucket;

		TexturePHashInfo() :
			raw_hash(0),
			canonical_hash(0),
			aspect_bucket(0),
			luma_bucket(0)
		{}
	};

	FlatHashMap<TexturePHashL2Key, uint64_t, TexturePHashL2KeyHasher> texture_phash_l2_cache(2048);
	std::vector<TexturePHashCanonicalEntry> texture_phash_l3_cache;

	static inline uint8_t expand_5_to_8(uint8_t v)
	{
		return (uint8_t)((v << 3) | (v >> 2));
	}

	static inline uint8_t expand_6_to_8(uint8_t v)
	{
		return (uint8_t)((v << 2) | (v >> 4));
	}

	template <typename T>
	static inline T phash_min(const T& a, const T& b)
	{
		return (a < b) ? a : b;
	}

	template <typename T>
	static inline T phash_max(const T& a, const T& b)
	{
		return (a > b) ? a : b;
	}

	static inline float clamp01(float v)
	{
		return phash_max(0.0f, phash_min(1.0f, v));
	}

	static inline float rgb_to_luma(float r, float g, float b)
	{
		return clamp01(r * 0.299f + g * 0.587f + b * 0.114f);
	}

	static int popcount64(uint64_t value)
	{
		int count = 0;
		while (value) {
			value &= (value - 1);
			count++;
		}
		return count;
	}

	static float half_to_float(uint16_t half)
	{
		uint16_t sign = (half >> 15) & 0x1;
		uint16_t exp = (half >> 10) & 0x1f;
		uint16_t mantissa = half & 0x3ff;

		if (!exp) {
			if (!mantissa)
				return sign ? -0.0f : 0.0f;
			float value = mantissa / 1024.0f;
			value = std::ldexp(value, -14);
			return sign ? -value : value;
		}

		if (exp == 0x1f) {
			if (!mantissa)
				return sign ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
			return std::numeric_limits<float>::quiet_NaN();
		}

		float value = 1.0f + mantissa / 1024.0f;
		value = std::ldexp(value, exp - 15);
		return sign ? -value : value;
	}

	static void decode_bc1_block(const uint8_t *block, float *out_luma)
	{
		uint16_t c0 = block[0] | (block[1] << 8);
		uint16_t c1 = block[2] | (block[3] << 8);
		uint8_t r0 = expand_5_to_8((uint8_t)((c0 >> 11) & 0x1f));
		uint8_t g0 = expand_6_to_8((uint8_t)((c0 >> 5) & 0x3f));
		uint8_t b0 = expand_5_to_8((uint8_t)(c0 & 0x1f));
		uint8_t r1 = expand_5_to_8((uint8_t)((c1 >> 11) & 0x1f));
		uint8_t g1 = expand_6_to_8((uint8_t)((c1 >> 5) & 0x3f));
		uint8_t b1 = expand_5_to_8((uint8_t)(c1 & 0x1f));
		float palette[4];

		palette[0] = rgb_to_luma(r0 / 255.0f, g0 / 255.0f, b0 / 255.0f);
		palette[1] = rgb_to_luma(r1 / 255.0f, g1 / 255.0f, b1 / 255.0f);

		if (c0 > c1) {
			palette[2] = (2.0f * palette[0] + palette[1]) / 3.0f;
			palette[3] = (palette[0] + 2.0f * palette[1]) / 3.0f;
		} else {
			palette[2] = (palette[0] + palette[1]) * 0.5f;
			palette[3] = 0.0f;
		}

		uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
		for (int i = 0; i < 16; i++) {
			out_luma[i] = palette[(indices >> (i * 2)) & 0x3];
		}
	}

	static void decode_bc2_alpha(const uint8_t *block, float *out_alpha)
	{
		for (int row = 0; row < 4; row++) {
			uint16_t alpha_row = block[row * 2] | (block[row * 2 + 1] << 8);
			for (int col = 0; col < 4; col++) {
				out_alpha[row * 4 + col] = ((alpha_row >> (col * 4)) & 0xf) / 15.0f;
			}
		}
	}

	static void decode_bc3_alpha(const uint8_t *block, float *out_alpha)
	{
		uint8_t a0 = block[0];
		uint8_t a1 = block[1];
		float palette[8];
		uint64_t alpha_bits = 0;

		palette[0] = a0 / 255.0f;
		palette[1] = a1 / 255.0f;
		if (a0 > a1) {
			for (int i = 1; i <= 6; i++) {
				palette[i + 1] = ((7 - i) * palette[0] + i * palette[1]) / 7.0f;
			}
		} else {
			for (int i = 1; i <= 4; i++) {
				palette[i + 1] = ((5 - i) * palette[0] + i * palette[1]) / 5.0f;
			}
			palette[6] = 0.0f;
			palette[7] = 1.0f;
		}

		for (int i = 0; i < 6; i++) {
			alpha_bits |= (uint64_t)block[2 + i] << (8 * i);
		}
		for (int i = 0; i < 16; i++) {
			out_alpha[i] = palette[(alpha_bits >> (i * 3)) & 0x7];
		}
	}

	static void decode_bc4_block(const uint8_t *block, float *out_values)
	{
		decode_bc3_alpha(block, out_values);
	}

	static bool decode_block_compressed_luma(DXGI_FORMAT format, const uint8_t *base, UINT row_pitch,
		UINT width, UINT height, UINT x, UINT y, PHashPixelSampleCache *cache, float *out_luma)
	{
		UINT block_x = x / 4;
		UINT block_y = y / 4;
		UINT blocks_per_row = (width + 3) / 4;
		UINT block_size = CompressedFormatBlockSize(format);
		const uint8_t *block;
		float alpha[16];
		float green[16];

		if (!block_size)
			return false;

		if (!cache->valid || cache->block_x != (int)block_x || cache->block_y != (int)block_y) {
			block = base + block_y * row_pitch + block_x * block_size;

			switch (format) {
				case DXGI_FORMAT_BC1_TYPELESS:
				case DXGI_FORMAT_BC1_UNORM:
				case DXGI_FORMAT_BC1_UNORM_SRGB:
					decode_bc1_block(block, cache->block_luma);
					break;
				case DXGI_FORMAT_BC2_TYPELESS:
				case DXGI_FORMAT_BC2_UNORM:
				case DXGI_FORMAT_BC2_UNORM_SRGB:
					decode_bc1_block(block + 8, cache->block_luma);
					decode_bc2_alpha(block, alpha);
					for (int i = 0; i < 16; i++)
						cache->block_luma[i] *= alpha[i];
					break;
				case DXGI_FORMAT_BC3_TYPELESS:
				case DXGI_FORMAT_BC3_UNORM:
				case DXGI_FORMAT_BC3_UNORM_SRGB:
					decode_bc1_block(block + 8, cache->block_luma);
					decode_bc3_alpha(block, alpha);
					for (int i = 0; i < 16; i++)
						cache->block_luma[i] *= alpha[i];
					break;
				case DXGI_FORMAT_BC4_TYPELESS:
				case DXGI_FORMAT_BC4_UNORM:
				case DXGI_FORMAT_BC4_SNORM:
					decode_bc4_block(block, cache->block_luma);
					break;
				case DXGI_FORMAT_BC5_TYPELESS:
				case DXGI_FORMAT_BC5_UNORM:
				case DXGI_FORMAT_BC5_SNORM:
					decode_bc4_block(block, cache->block_luma);
					decode_bc4_block(block + 8, green);
					for (int i = 0; i < 16; i++)
						cache->block_luma[i] = (cache->block_luma[i] + green[i]) * 0.5f;
					break;
				default:
					return false;
			}

			cache->block_x = (int)block_x;
			cache->block_y = (int)block_y;
			cache->valid = true;
		}

		if (block_x >= blocks_per_row || y >= height)
			return false;

		*out_luma = cache->block_luma[(y & 3) * 4 + (x & 3)];
		return true;
	}

	static bool sample_uncompressed_luma(DXGI_FORMAT format, const uint8_t *base, UINT row_pitch,
		UINT x, UINT y, float *out_luma)
	{
		const uint8_t *row = base + row_pitch * y;
		switch (format) {
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_A8_UNORM:
				*out_luma = row[x] / 255.0f;
				return true;
			case DXGI_FORMAT_R8G8_UNORM:
				*out_luma = (row[x * 2] + row[x * 2 + 1]) / (255.0f * 2.0f);
				return true;
			case DXGI_FORMAT_B5G6R5_UNORM:
			{
				uint16_t pixel = ((const uint16_t*)row)[x];
				float r = expand_5_to_8((uint8_t)((pixel >> 11) & 0x1f)) / 255.0f;
				float g = expand_6_to_8((uint8_t)((pixel >> 5) & 0x3f)) / 255.0f;
				float b = expand_5_to_8((uint8_t)(pixel & 0x1f)) / 255.0f;
				*out_luma = rgb_to_luma(r, g, b);
				return true;
			}
			case DXGI_FORMAT_B5G5R5A1_UNORM:
			{
				uint16_t pixel = ((const uint16_t*)row)[x];
				float r = expand_5_to_8((uint8_t)((pixel >> 10) & 0x1f)) / 255.0f;
				float g = expand_5_to_8((uint8_t)((pixel >> 5) & 0x1f)) / 255.0f;
				float b = expand_5_to_8((uint8_t)(pixel & 0x1f)) / 255.0f;
				float a = (pixel & 0x8000) ? 1.0f : 0.0f;
				*out_luma = rgb_to_luma(r, g, b) * a;
				return true;
			}
			case DXGI_FORMAT_B4G4R4A4_UNORM:
			{
				uint16_t pixel = ((const uint16_t*)row)[x];
				float b = ((pixel >> 0) & 0xf) / 15.0f;
				float g = ((pixel >> 4) & 0xf) / 15.0f;
				float r = ((pixel >> 8) & 0xf) / 15.0f;
				float a = ((pixel >> 12) & 0xf) / 15.0f;
				*out_luma = rgb_to_luma(r, g, b) * a;
				return true;
			}
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			{
				const uint8_t *pixel = row + x * 4;
				*out_luma = rgb_to_luma(pixel[2] / 255.0f, pixel[1] / 255.0f, pixel[0] / 255.0f) * (pixel[3] / 255.0f);
				return true;
			}
			case DXGI_FORMAT_B8G8R8X8_TYPELESS:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			{
				const uint8_t *pixel = row + x * 4;
				*out_luma = rgb_to_luma(pixel[2] / 255.0f, pixel[1] / 255.0f, pixel[0] / 255.0f);
				return true;
			}
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SNORM:
			case DXGI_FORMAT_R8G8B8A8_SINT:
			{
				const uint8_t *pixel = row + x * 4;
				*out_luma = rgb_to_luma(pixel[0] / 255.0f, pixel[1] / 255.0f, pixel[2] / 255.0f) * (pixel[3] / 255.0f);
				return true;
			}
			case DXGI_FORMAT_R16_UNORM:
			{
				*out_luma = ((const uint16_t*)row)[x] / 65535.0f;
				return true;
			}
			case DXGI_FORMAT_R16_FLOAT:
			{
				*out_luma = clamp01(half_to_float(((const uint16_t*)row)[x]));
				return true;
			}
			case DXGI_FORMAT_R16G16_FLOAT:
			{
				const uint16_t *pixel = ((const uint16_t*)row) + x * 2;
				*out_luma = clamp01((half_to_float(pixel[0]) + half_to_float(pixel[1])) * 0.5f);
				return true;
			}
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			{
				const uint16_t *pixel = ((const uint16_t*)row) + x * 4;
				*out_luma = rgb_to_luma(clamp01(half_to_float(pixel[0])),
					clamp01(half_to_float(pixel[1])),
					clamp01(half_to_float(pixel[2]))) * clamp01(half_to_float(pixel[3]));
				return true;
			}
			default:
				return false;
		}
	}

	static bool sample_texture2d_luma(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
		UINT x, UINT y, PHashPixelSampleCache *cache, float *out_luma)
	{
		if (!desc || !data || !data->pSysMem || x >= desc->Width || y >= desc->Height)
			return false;

		const uint8_t *base = (const uint8_t*)data->pSysMem;
		if (CompressedFormatBlockSize(desc->Format)) {
			return decode_block_compressed_luma(desc->Format, base, data->SysMemPitch,
				desc->Width, desc->Height, x, y, cache, out_luma);
		}

		return sample_uncompressed_luma(desc->Format, base, data->SysMemPitch, x, y, out_luma);
	}

	static bool bilinear_sample_texture2d_luma(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
		float x, float y, PHashPixelSampleCache *cache, float *out_luma)
	{
		UINT x0, y0, x1, y1;
		float fx, fy;
		float c00, c10, c01, c11;

		if (!desc || !data || !out_luma)
			return false;

		x = phash_max(0.0f, phash_min(x, (float)desc->Width - 1.0f));
		y = phash_max(0.0f, phash_min(y, (float)desc->Height - 1.0f));

		x0 = (UINT)floorf(x);
		y0 = (UINT)floorf(y);
		x1 = phash_min(x0 + 1, desc->Width - 1);
		y1 = phash_min(y0 + 1, desc->Height - 1);
		fx = x - x0;
		fy = y - y0;

		if (!sample_texture2d_luma(desc, data, x0, y0, cache, &c00)
		 || !sample_texture2d_luma(desc, data, x1, y0, cache, &c10)
		 || !sample_texture2d_luma(desc, data, x0, y1, cache, &c01)
		 || !sample_texture2d_luma(desc, data, x1, y1, cache, &c11))
			return false;

		*out_luma =
			(1.0f - fx) * (1.0f - fy) * c00 +
			fx * (1.0f - fy) * c10 +
			(1.0f - fx) * fy * c01 +
			fx * fy * c11;
		return true;
	}

	static bool resize_texture_luma(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
		UINT target_width, UINT target_height, std::vector<float> *out)
	{
		PHashPixelSampleCache cache;
		float src_x_scale;
		float src_y_scale;

		if (!out)
			return false;

		out->assign(target_width * target_height, 0.0f);
		if (!desc || !data || !data->pSysMem || !desc->Width || !desc->Height)
			return false;

		src_x_scale = (float)desc->Width / target_width;
		src_y_scale = (float)desc->Height / target_height;

		for (UINT y = 0; y < target_height; y++) {
			for (UINT x = 0; x < target_width; x++) {
				float luma;
				float sample_x = (x + 0.5f) * src_x_scale - 0.5f;
				float sample_y = (y + 0.5f) * src_y_scale - 0.5f;
				if (!bilinear_sample_texture2d_luma(desc, data, sample_x, sample_y, &cache, &luma))
					return false;
				(*out)[y * target_width + x] = luma;
			}
		}

		return true;
	}

	static void resize_grayscale_bilinear(const std::vector<float>& src, UINT src_width, UINT src_height,
		UINT dst_width, UINT dst_height, std::vector<float> *dst)
	{
		if (!dst)
			return;

		dst->assign(dst_width * dst_height, 0.0f);
		for (UINT y = 0; y < dst_height; y++) {
			float sample_y = ((y + 0.5f) * src_height / dst_height) - 0.5f;
			sample_y = phash_max(0.0f, phash_min(sample_y, (float)src_height - 1.0f));
			float sy0f = floorf(sample_y);
			UINT sy0 = (UINT)phash_min(sy0f, (float)src_height - 1.0f);
			UINT sy1 = phash_min(sy0 + 1, src_height - 1);
			float fy = sample_y - sy0;
			for (UINT x = 0; x < dst_width; x++) {
				float sample_x = ((x + 0.5f) * src_width / dst_width) - 0.5f;
				sample_x = phash_max(0.0f, phash_min(sample_x, (float)src_width - 1.0f));
				float sx0f = floorf(sample_x);
				UINT sx0 = (UINT)phash_min(sx0f, (float)src_width - 1.0f);
				UINT sx1 = phash_min(sx0 + 1, src_width - 1);
				float fx = sample_x - sx0;

				float c00 = src[sy0 * src_width + sx0];
				float c10 = src[sy0 * src_width + sx1];
				float c01 = src[sy1 * src_width + sx0];
				float c11 = src[sy1 * src_width + sx1];

				(*dst)[y * dst_width + x] =
					(1.0f - fx) * (1.0f - fy) * c00 +
					fx * (1.0f - fy) * c10 +
					(1.0f - fx) * fy * c01 +
					fx * fy * c11;
			}
		}
	}

	static uint64_t compute_dct_phash64(const std::vector<float>& normalized, uint8_t *out_luma_bucket)
	{
		static bool tables_initialized = false;
		static float cos_table[TEXTURE_PHASH_LOW_FREQ_EDGE][TEXTURE_PHASH_DCT_EDGE];
		std::array<float, TEXTURE_PHASH_LOW_FREQ_EDGE * TEXTURE_PHASH_LOW_FREQ_EDGE> coeffs;
		std::array<float, TEXTURE_PHASH_LOW_FREQ_EDGE * TEXTURE_PHASH_LOW_FREQ_EDGE - 1> threshold_values;
		float average_luma = 0.0f;
		uint64_t hash = 0;
		size_t coeff_index = 0;

		if (!tables_initialized) {
			for (UINT u = 0; u < TEXTURE_PHASH_LOW_FREQ_EDGE; u++) {
				for (UINT x = 0; x < TEXTURE_PHASH_DCT_EDGE; x++) {
					cos_table[u][x] = cosf(((2.0f * x + 1.0f) * u * TEXTURE_PHASH_PI) / (2.0f * TEXTURE_PHASH_DCT_EDGE));
				}
			}
			tables_initialized = true;
		}

		for (float value : normalized)
			average_luma += value;
		average_luma /= (float)normalized.size();
		if (out_luma_bucket)
			*out_luma_bucket = (uint8_t)phash_min(255.0f, floorf(average_luma * 255.0f));

		for (UINT v = 0; v < TEXTURE_PHASH_LOW_FREQ_EDGE; v++) {
			for (UINT u = 0; u < TEXTURE_PHASH_LOW_FREQ_EDGE; u++) {
				float sum = 0.0f;
				float alpha_u = (u == 0) ? (1.0f / sqrtf((float)TEXTURE_PHASH_DCT_EDGE)) : sqrtf(2.0f / TEXTURE_PHASH_DCT_EDGE);
				float alpha_v = (v == 0) ? (1.0f / sqrtf((float)TEXTURE_PHASH_DCT_EDGE)) : sqrtf(2.0f / TEXTURE_PHASH_DCT_EDGE);

				for (UINT y = 0; y < TEXTURE_PHASH_DCT_EDGE; y++) {
					for (UINT x = 0; x < TEXTURE_PHASH_DCT_EDGE; x++) {
						sum += normalized[y * TEXTURE_PHASH_DCT_EDGE + x] * cos_table[u][x] * cos_table[v][y];
					}
				}
				coeffs[coeff_index++] = sum * alpha_u * alpha_v;
			}
		}

		for (size_t i = 1; i < coeffs.size(); i++)
			threshold_values[i - 1] = coeffs[i];
		std::nth_element(threshold_values.begin(),
			threshold_values.begin() + threshold_values.size() / 2,
			threshold_values.end());
		float median = threshold_values[threshold_values.size() / 2];

		for (size_t i = 0; i < coeffs.size(); i++) {
			if (coeffs[i] >= median)
				hash |= (1ull << i);
		}
		return hash;
	}

	static uint16_t get_aspect_bucket(UINT width, UINT height)
	{
		if (!width || !height)
			return 0;
		return (uint16_t)phash_min(65535u, (UINT)((width * 256ull) / phash_max(height, 1u)));
	}

	static uint64_t canonicalize_texture_phash(uint64_t raw_hash, uint16_t aspect_bucket, uint8_t luma_bucket)
	{
		TexturePHashL2Key key{ raw_hash, aspect_bucket, luma_bucket };
		uint64_t canonical_hash = raw_hash;

		if (!raw_hash)
			return 0;

		EnterCriticalSectionPretty(&G->mCriticalSection);

		if (uint64_t *cached = texture_phash_l2_cache.find_ptr(key)) {
			canonical_hash = *cached;
			LeaveCriticalSection(&G->mCriticalSection);
			return canonical_hash;
		}

		for (TexturePHashCanonicalEntry &entry : texture_phash_l3_cache) {
			if (entry.aspect_bucket != aspect_bucket)
				continue;
			if (std::abs((int)entry.luma_bucket - (int)luma_bucket) > 8)
				continue;
			if (popcount64(entry.canonical_hash ^ raw_hash) <= TEXTURE_PHASH_MAX_HAMMING_DISTANCE) {
				canonical_hash = entry.canonical_hash;
				break;
			}
		}

		if (canonical_hash == raw_hash) {
			texture_phash_l3_cache.push_back({ raw_hash, aspect_bucket, luma_bucket });
		}
		texture_phash_l2_cache.insert(key, canonical_hash);

		LeaveCriticalSection(&G->mCriticalSection);
		return canonical_hash;
	}

	static bool compute_texture2d_phash_info(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *data,
		TexturePHashInfo *out_info)
	{
		std::vector<float> normalized;
		std::vector<float> dct_input;
		uint8_t luma_bucket = 0;

		if (!out_info || !desc || !data || !data->pSysMem)
			return false;

		if (!resize_texture_luma(desc, data, TEXTURE_PHASH_NORMALIZED_EDGE, TEXTURE_PHASH_NORMALIZED_EDGE, &normalized))
			return false;

		resize_grayscale_bilinear(normalized, TEXTURE_PHASH_NORMALIZED_EDGE, TEXTURE_PHASH_NORMALIZED_EDGE,
			TEXTURE_PHASH_DCT_EDGE, TEXTURE_PHASH_DCT_EDGE, &dct_input);

		out_info->raw_hash = compute_dct_phash64(dct_input, &luma_bucket);
		out_info->aspect_bucket = get_aspect_bucket(desc->Width, desc->Height);
		out_info->luma_bucket = luma_bucket;
		out_info->canonical_hash = canonicalize_texture_phash(out_info->raw_hash, out_info->aspect_bucket, out_info->luma_bucket);
		return !!out_info->canonical_hash;
	}
}

// Overloaded functions to log any kind of resource description (useful to call
// from templates):

static wstring TexBindFlags(UINT bind_flags)
{
	if (bind_flags)
		return L"bind_flags=\"" + lookup_enum_bit_names(CustomResourceBindFlagNames, (CustomResourceBindFlags)bind_flags) + L"\"";
	return L"bind_flags=0";
}

static wstring TexCPUFlags(UINT cpu_flags)
{
	if (cpu_flags)
		return L"cpu_access_flags=\"" + lookup_enum_bit_names(ResourceCPUAccessFlagNames, (ResourceCPUAccessFlags)cpu_flags) + L"\"";
	return L"cpu_access_flags=0";
}

static wstring TexMiscFlags(UINT misc_flags)
{
	if (misc_flags)
		return L"misc_flags=\"" + lookup_enum_bit_names(ResourceMiscFlagNames, (ResourceMiscFlags)misc_flags) + L"\"";
	return L"misc_flags=0";
}

int StrResourceDesc(char *buf, size_t size, const D3D11_BUFFER_DESC *desc)
{
	return _snprintf_s(buf, size, size, "type=Buffer byte_width=%u "
		"usage=\"%S\" %S %S %S stride=%u",
		desc->ByteWidth, TexResourceUsage(desc->Usage),
		TexBindFlags(desc->BindFlags).c_str(),
		TexCPUFlags(desc->CPUAccessFlags).c_str(),
		TexMiscFlags(desc->MiscFlags).c_str(),
		desc->StructureByteStride);
}

int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE1D_DESC *desc)
{
	return _snprintf_s(buf, size, size, "type=Texture1D width=%u mips=%u "
		"array=%u format=\"%s\" usage=\"%S\" %S %S %S",
		desc->Width, desc->MipLevels, desc->ArraySize,
		TexFormatStr(desc->Format), TexResourceUsage(desc->Usage),
		TexBindFlags(desc->BindFlags).c_str(),
		TexCPUFlags(desc->CPUAccessFlags).c_str(),
		TexMiscFlags(desc->MiscFlags).c_str());
}

int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE2D_DESC *desc)
{
	return _snprintf_s(buf, size, size, "type=Texture2D width=%u height=%u mips=%u "
		"array=%u format=\"%s\" msaa=%u "
		"msaa_quality=%u usage=\"%S\" %S %S %S",
		desc->Width, desc->Height, desc->MipLevels, desc->ArraySize,
		TexFormatStr(desc->Format), desc->SampleDesc.Count,
		desc->SampleDesc.Quality, TexResourceUsage(desc->Usage),
		TexBindFlags(desc->BindFlags).c_str(),
		TexCPUFlags(desc->CPUAccessFlags).c_str(),
		TexMiscFlags(desc->MiscFlags).c_str());
}

int StrResourceDesc(char *buf, size_t size, const D3D11_TEXTURE3D_DESC *desc)
{
	return _snprintf_s(buf, size, size, "type=Texture3D width=%u height=%u depth=%u "
		"mips=%u format=\"%s\" usage=\"%S\" %S %S %S",
		desc->Width, desc->Height, desc->Depth, desc->MipLevels,
		TexFormatStr(desc->Format), TexResourceUsage(desc->Usage),
		TexBindFlags(desc->BindFlags).c_str(),
		TexCPUFlags(desc->CPUAccessFlags).c_str(),
		TexMiscFlags(desc->MiscFlags).c_str());
}

int StrResourceDesc(char *buf, size_t size, struct ResourceHashInfo &info)
{
	switch (info.type) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			return StrResourceDesc(buf, size, &info.buf_desc);
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			return StrResourceDesc(buf, size, &info.tex1d_desc);
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			return StrResourceDesc(buf, size, &info.tex2d_desc);
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			return StrResourceDesc(buf, size, &info.tex3d_desc);
		default:
			return _snprintf_s(buf, size, size, "type=%i", info.type);
	}
}

template <typename DescType>
static void LogResourceDescCommon(DescType *desc)
{
	LogInfo("    Usage = %d\n", desc->Usage);
	LogInfo("    BindFlags = 0x%x\n", desc->BindFlags);
	LogInfo("    CPUAccessFlags = 0x%x\n", desc->CPUAccessFlags);
	LogInfo("    MiscFlags = 0x%x\n", desc->MiscFlags);
}

void LogResourceDesc(const D3D11_BUFFER_DESC *desc)
{
	LogInfo("  Resource Type = Buffer\n");
	LogInfo("    ByteWidth = %d\n", desc->ByteWidth);
	LogResourceDescCommon(desc);
	LogInfo("    StructureByteStride = %d\n", desc->StructureByteStride);
}

void LogResourceDesc(const D3D11_TEXTURE1D_DESC *desc)
{
	LogInfo("  Resource Type = Texture1D\n");
	LogInfo("    Width = %d\n", desc->Width);
	LogInfo("    MipLevels = %d\n", desc->MipLevels);
	LogInfo("    ArraySize = %d\n", desc->ArraySize);
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	LogResourceDescCommon(desc);
}

void LogResourceDesc(const D3D11_TEXTURE2D_DESC *desc)
{
	LogInfo("  Resource Type = Texture2D\n");
	LogInfo("    Width = %d\n", desc->Width);
	LogInfo("    Height = %d\n", desc->Height);
	LogInfo("    MipLevels = %d\n", desc->MipLevels);
	LogInfo("    ArraySize = %d\n", desc->ArraySize);
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	LogInfo("    SampleDesc.Count = %d\n", desc->SampleDesc.Count);
	LogInfo("    SampleDesc.Quality = %d\n", desc->SampleDesc.Quality);
	LogResourceDescCommon(desc);
}

void LogResourceDesc(const D3D11_TEXTURE3D_DESC *desc)
{
	LogInfo("  Resource Type = Texture3D\n");
	LogInfo("    Width = %d\n", desc->Width);
	LogInfo("    Height = %d\n", desc->Height);
	LogInfo("    Depth = %d\n", desc->Depth);
	LogInfo("    MipLevels = %d\n", desc->MipLevels);
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	LogResourceDescCommon(desc);
}

void LogResourceDesc(ID3D11Resource *resource)
{
	D3D11_RESOURCE_DIMENSION dim;
	ID3D11Buffer *buffer;
	ID3D11Texture1D *tex_1d;
	ID3D11Texture2D *tex_2d;
	ID3D11Texture3D *tex_3d;
	D3D11_BUFFER_DESC buffer_desc;
	D3D11_TEXTURE1D_DESC desc_1d;
	D3D11_TEXTURE2D_DESC desc_2d;
	D3D11_TEXTURE3D_DESC desc_3d;

	resource->GetType(&dim);
	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			buffer = (ID3D11Buffer*)resource;
			buffer->GetDesc(&buffer_desc);
			return LogResourceDesc(&buffer_desc);
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			tex_1d = (ID3D11Texture1D*)resource;
			tex_1d->GetDesc(&desc_1d);
			return LogResourceDesc(&desc_1d);
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex_2d = (ID3D11Texture2D*)resource;
			tex_2d->GetDesc(&desc_2d);
			return LogResourceDesc(&desc_2d);
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex_3d = (ID3D11Texture3D*)resource;
			tex_3d->GetDesc(&desc_3d);
			return LogResourceDesc(&desc_3d);
	}
}

void LogViewDesc(const D3D11_SHADER_RESOURCE_VIEW_DESC *desc)
{
	LogInfo("  View Type = Shader Resource\n");
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	switch (desc->ViewDimension) {
		case D3D11_SRV_DIMENSION_UNKNOWN:
			LogInfo("    ViewDimension = UNKNOWN\n");
			break;
		case D3D11_SRV_DIMENSION_BUFFER:
			LogInfo("    ViewDimension = BUFFER\n");
			LogInfo("      Buffer.FirstElement/NumElements = %u\n", desc->Buffer.FirstElement);
			LogInfo("      Buffer.ElementOffset/ElementWidth = %u\n", desc->Buffer.ElementOffset);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE1D:
			LogInfo("    ViewDimension = TEXTURE1D\n");
			LogInfo("      Texture1D.MostDetailedMip = %u\n", desc->Texture1D.MostDetailedMip);
			LogInfo("      Texture1D.MipLevels = %d\n", desc->Texture1D.MipLevels);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
			LogInfo("    ViewDimension = TEXTURE1DARRAY\n");
			LogInfo("      Texture1DArray.MostDetailedMip = %u\n", desc->Texture1DArray.MostDetailedMip);
			LogInfo("      Texture1DArray.MipLevels = %d\n", desc->Texture1DArray.MipLevels);
			LogInfo("      Texture1DArray.FirstArraySlice = %u\n", desc->Texture1DArray.FirstArraySlice);
			LogInfo("      Texture1DArray.ArraySize = %u\n", desc->Texture1DArray.ArraySize);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE2D:
			LogInfo("    ViewDimension = TEXTURE2D\n");
			LogInfo("      Texture2D.MostDetailedMip = %u\n", desc->Texture2D.MostDetailedMip);
			LogInfo("      Texture2D.MipLevels = %d\n", desc->Texture2D.MipLevels);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
			LogInfo("    ViewDimension = TEXTURE2DARRAY\n");
			LogInfo("      Texture2DArray.MostDetailedMip = %u\n", desc->Texture2DArray.MostDetailedMip);
			LogInfo("      Texture2DArray.MipLevels = %d\n", desc->Texture2DArray.MipLevels);
			LogInfo("      Texture2DArray.FirstArraySlice = %u\n", desc->Texture2DArray.FirstArraySlice);
			LogInfo("      Texture2DArray.ArraySize = %u\n", desc->Texture2DArray.ArraySize);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE2DMS:
			LogInfo("    ViewDimension = TEXTURE2DMS\n");
			break;
		case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
			LogInfo("    ViewDimension = TEXTURE2DMSARRAY\n");
			LogInfo("      Texture2DMSArray.FirstArraySlice = %u\n", desc->Texture2DMSArray.FirstArraySlice);
			LogInfo("      Texture2DMSArray.ArraySize = %u\n", desc->Texture2DMSArray.ArraySize);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE3D:
			LogInfo("    ViewDimension = TEXTURE3D\n");
			LogInfo("      Texture3D.MostDetailedMip = %u\n", desc->Texture3D.MostDetailedMip);
			LogInfo("      Texture3D.MipLevels = %d\n", desc->Texture3D.MipLevels);
			break;
		case D3D11_SRV_DIMENSION_TEXTURECUBE:
			LogInfo("    ViewDimension = TEXTURECUBE\n");
			LogInfo("      TextureCube.MostDetailedMip = %u\n", desc->TextureCube.MostDetailedMip);
			LogInfo("      TextureCube.MipLevels = %d\n", desc->TextureCube.MipLevels);
			break;
		case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
			LogInfo("    ViewDimension = TEXTURECUBEARRAY\n");
			LogInfo("      TextureCubeArray.MostDetailedMip = %u\n", desc->TextureCubeArray.MostDetailedMip);
			LogInfo("      TextureCubeArray.MipLevels = %d\n", desc->TextureCubeArray.MipLevels);
			LogInfo("      TextureCubeArray.First2DArrayFace = %u\n", desc->TextureCubeArray.First2DArrayFace);
			LogInfo("      TextureCubeArray.NumCubes = %u\n", desc->TextureCubeArray.NumCubes);
			break;
		case D3D11_SRV_DIMENSION_BUFFEREX:
			LogInfo("    ViewDimension = BUFFEREX\n");
			LogInfo("      BufferEx.FirstElement = %u\n", desc->BufferEx.FirstElement);
			LogInfo("      BufferEx.NumElements = %u\n", desc->BufferEx.NumElements);
			LogInfo("      BufferEx.Flags = 0x%x\n", desc->BufferEx.Flags);
			break;
	}
}

void LogViewDesc(const D3D11_RENDER_TARGET_VIEW_DESC *desc)
{
	LogInfo("  View Type = Render Target\n");
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	switch (desc->ViewDimension) {
		case D3D11_RTV_DIMENSION_UNKNOWN:
			LogInfo("    ViewDimension = UNKNOWN\n");
			break;
		case D3D11_RTV_DIMENSION_BUFFER:
			LogInfo("    ViewDimension = BUFFER\n");
			LogInfo("      Buffer.FirstElement/NumElements = %u\n", desc->Buffer.FirstElement);
			LogInfo("      Buffer.ElementOffset/ElementWidth = %u\n", desc->Buffer.ElementOffset);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE1D:
			LogInfo("    ViewDimension = TEXTURE1D\n");
			LogInfo("      Texture1D.MipSlice = %u\n", desc->Texture1D.MipSlice);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
			LogInfo("    ViewDimension = TEXTURE1DARRAY\n");
			LogInfo("      Texture1DArray.MipSlice = %u\n", desc->Texture1DArray.MipSlice);
			LogInfo("      Texture1DArray.FirstArraySlice = %u\n", desc->Texture1DArray.FirstArraySlice);
			LogInfo("      Texture1DArray.ArraySize = %u\n", desc->Texture1DArray.ArraySize);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2D:
			LogInfo("    ViewDimension = TEXTURE2D\n");
			LogInfo("      Texture2D.MipSlice = %u\n", desc->Texture2D.MipSlice);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
			LogInfo("    ViewDimension = TEXTURE2DARRAY\n");
			LogInfo("      Texture2DArray.MipSlice = %u\n", desc->Texture2DArray.MipSlice);
			LogInfo("      Texture2DArray.FirstArraySlice = %u\n", desc->Texture2DArray.FirstArraySlice);
			LogInfo("      Texture2DArray.ArraySize = %u\n", desc->Texture2DArray.ArraySize);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMS:
			LogInfo("    ViewDimension = TEXTURE2DMS\n");
			break;
		case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
			LogInfo("    ViewDimension = TEXTURE2DMSARRAY\n");
			LogInfo("      Texture2DMSArray.FirstArraySlice = %u\n", desc->Texture2DMSArray.FirstArraySlice);
			LogInfo("      Texture2DMSArray.ArraySize = %u\n", desc->Texture2DMSArray.ArraySize);
			break;
		case D3D11_RTV_DIMENSION_TEXTURE3D:
			LogInfo("    ViewDimension = TEXTURE3D\n");
			LogInfo("      Texture3D.MipSlice = %u\n", desc->Texture3D.MipSlice);
			LogInfo("      Texture3D.FirstWSlice = %u\n", desc->Texture3D.FirstWSlice);
			LogInfo("      Texture3D.WSize = %u\n", desc->Texture3D.WSize);
			break;
	}
}

void LogViewDesc(const D3D11_DEPTH_STENCIL_VIEW_DESC *desc)
{
	LogInfo("  View Type = Depth Stencil\n");
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	LogInfo("    Flags = 0x%x\n", desc->Flags);
	switch (desc->ViewDimension) {
		case D3D11_DSV_DIMENSION_UNKNOWN:
			LogInfo("    ViewDimension = UNKNOWN\n");
			break;
		case D3D11_DSV_DIMENSION_TEXTURE1D:
			LogInfo("    ViewDimension = TEXTURE1D\n");
			LogInfo("      Texture1D.MipSlice = %u\n", desc->Texture1D.MipSlice);
			break;
		case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
			LogInfo("    ViewDimension = TEXTURE1DARRAY\n");
			LogInfo("      Texture1DArray.MipSlice = %u\n", desc->Texture1DArray.MipSlice);
			LogInfo("      Texture1DArray.FirstArraySlice = %u\n", desc->Texture1DArray.FirstArraySlice);
			LogInfo("      Texture1DArray.ArraySize = %u\n", desc->Texture1DArray.ArraySize);
			break;
		case D3D11_DSV_DIMENSION_TEXTURE2D:
			LogInfo("    ViewDimension = TEXTURE2D\n");
			LogInfo("      Texture2D.MipSlice = %u\n", desc->Texture2D.MipSlice);
			break;
		case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
			LogInfo("    ViewDimension = TEXTURE2DARRAY\n");
			LogInfo("      Texture2DArray.MipSlice = %u\n", desc->Texture2DArray.MipSlice);
			LogInfo("      Texture2DArray.FirstArraySlice = %u\n", desc->Texture2DArray.FirstArraySlice);
			LogInfo("      Texture2DArray.ArraySize = %u\n", desc->Texture2DArray.ArraySize);
			break;
		case D3D11_DSV_DIMENSION_TEXTURE2DMS:
			LogInfo("    ViewDimension = TEXTURE2DMS\n");
			break;
		case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
			LogInfo("    ViewDimension = TEXTURE2DMSARRAY\n");
			LogInfo("      Texture2DMSArray.FirstArraySlice = %u\n", desc->Texture2DMSArray.FirstArraySlice);
			LogInfo("      Texture2DMSArray.ArraySize = %u\n", desc->Texture2DMSArray.ArraySize);
			break;
	}
}

void LogViewDesc(const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc)
{
	LogInfo("  View Type = Unordered Access\n");
	LogInfo("    Format = %s (%d)\n", TexFormatStr(desc->Format), desc->Format);
	switch (desc->ViewDimension) {
		case D3D11_UAV_DIMENSION_UNKNOWN:
			LogInfo("    ViewDimension = UNKNOWN\n");
			break;
		case D3D11_UAV_DIMENSION_BUFFER:
			LogInfo("    ViewDimension = BUFFER\n");
			LogInfo("      Buffer.FirstElement = %u\n", desc->Buffer.FirstElement);
			LogInfo("      Buffer.NumElements = %u\n", desc->Buffer.NumElements);
			LogInfo("      Buffer.Flags = 0x%x\n", desc->Buffer.Flags);
			break;
		case D3D11_UAV_DIMENSION_TEXTURE1D:
			LogInfo("    ViewDimension = TEXTURE1D\n");
			LogInfo("      Texture1D.MipSlice = %u\n", desc->Texture1D.MipSlice);
			break;
		case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
			LogInfo("    ViewDimension = TEXTURE1DARRAY\n");
			LogInfo("      Texture1DArray.MipSlice = %u\n", desc->Texture1DArray.MipSlice);
			LogInfo("      Texture1DArray.FirstArraySlice = %u\n", desc->Texture1DArray.FirstArraySlice);
			LogInfo("      Texture1DArray.ArraySize = %u\n", desc->Texture1DArray.ArraySize);
			break;
		case D3D11_UAV_DIMENSION_TEXTURE2D:
			LogInfo("    ViewDimension = TEXTURE2D\n");
			LogInfo("      Texture2D.MipSlice = %u\n", desc->Texture2D.MipSlice);
			break;
		case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
			LogInfo("    ViewDimension = TEXTURE2DARRAY\n");
			LogInfo("      Texture2DArray.MipSlice = %u\n", desc->Texture2DArray.MipSlice);
			LogInfo("      Texture2DArray.FirstArraySlice = %u\n", desc->Texture2DArray.FirstArraySlice);
			LogInfo("      Texture2DArray.ArraySize = %u\n", desc->Texture2DArray.ArraySize);
			break;
		case D3D11_UAV_DIMENSION_TEXTURE3D:
			LogInfo("    ViewDimension = TEXTURE3D\n");
			LogInfo("      Texture3D.MipSlice = %u\n", desc->Texture3D.MipSlice);
			LogInfo("      Texture3D.FirstWSlice = %u\n", desc->Texture3D.FirstWSlice);
			LogInfo("      Texture3D.WSize = %u\n", desc->Texture3D.WSize);
			break;
	}
}


// This special case of texture resolution is to improve the behavior of special 
// full-screen textures.  Textures can be created dynamically of course, and some
// are set to full screen resolution.  Full screen resolution can vary between
// users and we want a way to have a stable texture hash, even while the screen
// resolution is varying.  
//
// This function will modify the hashWidth and hashHeight values actually used
// in the hash calculation to be magic numbers, really just constants.  That 
// will make the hash predictable and match, even if the screen resolution changes.
//
// The other variants are for *2, *4, *8, /2, as other textures seen with specific
// resolutions, but are also dynamic based on screen resolution, like 2x or 1/2 the
// resolution.  
//
// ToDo: It might make more sense to avoid this altogether, and have the shaderhacker
// specify their desired texture in the d3dx.ini file by parameters, not by a single
// hash.  That would be a sequence found via the ShaderUsages that would specify all
// the parameters in something like the D3D11_TEXTURE2D_DESC.
// The only drawback here is to make it more complicated for the shaderhacker, having
// to specify the little niggly bits, and requiring them to understand and look for 
// the alternate sizes. 
//
// If this seems like an OK way to go, what about other interesting magic combos like
// 1.5x (720p->1080p), maybe 1080p specifically. 720/1080=2/3.
// Would it maybe make more sense to just do all the logical screen combos instead?

static void AdjustForConstResolution(UINT *hashWidth, UINT *hashHeight)
{
	int width = *hashWidth;
	int height = *hashHeight;

	if (G->mResolutionInfo.from == GetResolutionFrom::INVALID)
		return;

	if (width == G->mResolutionInfo.width && height == G->mResolutionInfo.height) {
		*hashWidth = 'SRES';
		*hashHeight = 'SRES';
	}
	else if (width == G->mResolutionInfo.width * 2 && height == G->mResolutionInfo.height * 2) {
		*hashWidth = 'SR*2';
		*hashHeight = 'SR*2';
	}
	else if (width == G->mResolutionInfo.width * 4 && height == G->mResolutionInfo.height * 4) {
		*hashWidth = 'SR*4';
		*hashHeight = 'SR*4';
	}
	else if (width == G->mResolutionInfo.width * 8 && height == G->mResolutionInfo.height * 8) {
		*hashWidth = 'SR*8';
		*hashHeight = 'SR*8';
	}
	else if (width == G->mResolutionInfo.width / 2 && height == G->mResolutionInfo.height / 2) {
		*hashWidth = 'SR/2';
		*hashHeight = 'SR/2';
	}
}

uint32_t CalcTexture2DDescHash(uint32_t initial_hash, const D3D11_TEXTURE2D_DESC *const_desc)
{
	// It concerns me that CreateTextureND can use an override if it
	// matches screen resolution, but when we record render target / shader
	// resource stats we don't use the same override.
	//
	// For textures made with CreateTextureND and later used as a render
	// target it's probably fine since the hash will still be stored, but
	// it could be a problem if we need the hash of a render target not
	// created directly with that. I don't know enough about the DX11 API
	// to know if this is an issue, but it might be worth using the screen
	// resolution override in all cases. -DarkStarSword

	// Based on that concern, and the need to have a pointer to the 
	// D3D11_TEXTURE2D_DESC struct for hash calculation, let's go ahead
	// and use the resolution override always.

	D3D11_TEXTURE2D_DESC* desc = const_cast<D3D11_TEXTURE2D_DESC*>(const_desc);

	UINT saveWidth = desc->Width;
	UINT saveHeight = desc->Height;
	AdjustForConstResolution(&desc->Width, &desc->Height);

	uint32_t hash = crc32c_hw(initial_hash, desc, sizeof(D3D11_TEXTURE2D_DESC));

	desc->Width = saveWidth;
	desc->Height = saveHeight;

	return hash;
}

uint32_t CalcTexture3DDescHash(uint32_t initial_hash, const D3D11_TEXTURE3D_DESC *const_desc)
{
	// Same comment as in CalcTexture2DDescHash above - concerned about
	// inconsistent use of these resolution overrides

	D3D11_TEXTURE3D_DESC* desc = const_cast<D3D11_TEXTURE3D_DESC*>(const_desc);

	UINT saveWidth = desc->Width;
	UINT saveHeight = desc->Height;
	AdjustForConstResolution(&desc->Width, &desc->Height);

	uint32_t hash = crc32c_hw(initial_hash, desc, sizeof(D3D11_TEXTURE3D_DESC));

	desc->Width = saveWidth;
	desc->Height = saveHeight;

	return hash;
}

// -----------------------------------------------------------------------------------------------

static UINT CompressedFormatBlockSize(DXGI_FORMAT Format)
{
	switch (Format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return 8;

		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return 16;
	}

	return 0;
}

static size_t Texture1DLength(
	const D3D11_TEXTURE1D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	UINT level)
{
	// At the moment we are only using the first mip-map level, but this
	// should work if we wanted to use another:
	UINT mip_width = max(pDesc->Width >> level, 1);

	// For Texture1Ds we can't use the row pitch, so we have to calculate
	// the size ourselves based on the format size and mip-map width. This
	// will return 0 if the texture is using some esoteric format. I don't
	// think block compressed formats work on 1D textures because those
	// operate on 4x4 blocks of pixels.
	return dxgi_format_size(pDesc->Format) * mip_width;
}

static size_t Texture2DLength(
	const D3D11_TEXTURE2D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	UINT level)
{
	UINT block_size, padded_width, padded_height;

	// We might simply be able to use SysMemSlicePitch. The documentation
	// indicates that it has "no meaning" for a 2D texture, but then in the
	// Remarks section indicates that it should be set to "the size of the
	// entire 2D surface in bytes"... but somehow I don't trust it - after
	// all, we don't set it when creating the stereo texture and that works!
	// https://msdn.microsoft.com/en-us/library/windows/desktop/ff476220(v=vs.85).aspx

	// At the moment we are only using the first mip-map level, but this
	// should work if we wanted to use another:
	UINT mip_width = max(pDesc->Width >> level, 1);
	UINT mip_height = max(pDesc->Height >> level, 1);

	block_size = CompressedFormatBlockSize(pDesc->Format);

	if (!block_size) {
		// Uncompressed texture - use the SysMemPitch to get
		// the width (including any padding) in bytes.
		return pInitialData->SysMemPitch * mip_height;
	}

	// In the case of compressed textures, we can't necessarily rely on
	// SysMemPitch because "lines" are meaningless until the texture has
	// been decompressed. Instead use the mip-map width + height padded to
	// a multiple of 4 with the 4x4 block size.

	padded_width = (mip_width + 3) & ~0x3;
	padded_height = (mip_height + 3) & ~0x3;

	return padded_width * padded_height / 16 * block_size;
}

static size_t Texture3DLength(
	const D3D11_TEXTURE3D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	UINT level)
{
	UINT block_size, padded_width, padded_height;

	// At the moment we are only using the first mip-map level, but this
	// should work if we wanted to use another:
	UINT mip_width = max(pDesc->Width >> level, 1);
	UINT mip_height = max(pDesc->Height >> level, 1);
	UINT mip_depth = max(pDesc->Depth >> level, 1);

	block_size = CompressedFormatBlockSize(pDesc->Format);

	if (!block_size) {
		// Uncompressed texture - use the SysMemSlicePitch to get the
		// width*height (including any padding) in bytes.
		return pInitialData->SysMemSlicePitch * mip_depth;
	}

	// Not sure if SysMemSlicePitch is reliable for compressed 3D textures.
	// Use the mip-map width, height + depth padded to a multiple of 4 with
	// the 4x4 block size.

	padded_width = (mip_width + 3) & ~0x3;
	padded_height = (mip_height + 3) & ~0x3;

	return padded_width * padded_height * mip_depth / 16 * block_size;
}

static uint32_t hash_tex2d_data(uint32_t hash, const void *data, size_t length,
		const D3D11_TEXTURE2D_DESC *pDesc, bool zero_padding,
		bool skip_padding, UINT mapped_row_pitch)
{
	size_t row_pitch, slice_pitch, row_count;

	// Each row in a 2D texture has some alignment constraint, and the
	// unused bytes at the end of each row can be garbage, interfering with
	// the hash calculation. We should probably have always been discarding
	// these bytes for the hash calculations, but it wasn't easily apparent
	// that would be necessary and now too many fixes depend on it to just
	// change it, but we might consider adding an option to do this.
	//
	// However, these garbage bytes are proven to interfere with frame
	// analysis de-duplication - not fatally so, but they do mess up the
	// hashes on many resources so the hashes are not fully de-duped
	// (easily observable dumping HUD textures in DOAXVV twice in a row and
	// many of the de-duped hashes will have changed).
	//
	// Replacing the padding bytes with zeroes makes the hashes consistent
	// and fixes about half the hashes to match the texture hashes of those
	// that should, however the other half are still incorrect (but
	// consistent at least) and further investigation is required.
	//
	// Two possibilities come to mind to investigate:
	// - The textures may have been created with garbage in the padding
	//   bytes that we ideally should ignore.
	// - The SysMemPitch used to create the resources may not be preserved
	//   by DirectX, so the RowPitch we use here may not match leading to
	//   the zero hash being incorrect. Ideally we would skip the padding
	//   rather than replace it with zeroes.
	//
	// This is based partially from DirectXTK's SaveDDSTextureToFile, but
	// with the length capped based on our length calculation, and with the
	// padding replaced with zeroes rather than skipped.

	if (!zero_padding && !skip_padding)
		return crc32c_hw(hash, data, length);

	DirectX::LoaderHelpers::GetSurfaceInfo(pDesc->Width, pDesc->Height, pDesc->Format, &slice_pitch, &row_pitch, &row_count);

	uint8_t *sptr = (uint8_t*)data;
	size_t msize = min(row_pitch, mapped_row_pitch);

	signed padding = (signed)mapped_row_pitch - (signed)row_pitch;
	uint8_t *zeroes = NULL;
	if (zero_padding && padding > 0) {
		zeroes = new uint8_t[padding];
		memset(zeroes, 0, padding);
	}

	signed remaining = (signed)length;
	for (size_t h = 0; h < row_count && remaining > 0; h++) {
		hash = crc32c_hw(hash, sptr, min(msize, (unsigned)remaining));
		sptr += mapped_row_pitch;
		remaining -= (signed)msize;

		if (zeroes && remaining > 0) {
			hash = crc32c_hw(hash, zeroes, min(padding, remaining));
			remaining -= padding;
		}
	}

	delete [] zeroes;
	return hash;
}

uint32_t CalcTexture2DDataHash(
	const D3D11_TEXTURE2D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	bool zero_padding)
{
	uint32_t hash = 0;
	size_t length_v12;
	size_t length;

	if (!pDesc || !pInitialData || !pInitialData->pSysMem)
		return 0;

	if (G->texture_hash_version)
		return CalcTexture2DDataHashAccurate(pDesc, pInitialData);

	// In 3DMigoto v1.2, this is what we were using as the length of the
	// buffer in bytes. Unfortunately this is not right since pDesc->Width
	// is in texels, not bytes, and if pDesc->ArraySize was greater than 1
	// it signifies that there are additional separate buffers to consider,
	// while we treated it as making the first buffer longer. Additionally,
	// compressed textures complicate the buffer size calculation further.
	//
	// The result is that we might not consider the entire buffer when
	// calculating the hash (which may not be ideal, but it is generally
	// acceptable), or we might overflow the buffer. If we overflow we
	// might get an exception if there is nothing mapped after the buffer
	// (which we catch and log), but we could just as easily process
	// gargage after the buffer as being part of the texture, which would
	// lead to us creating unpredictable hashes.
	length_v12 = pDesc->Width * pDesc->Height * pDesc->ArraySize;

	// Compare the old broken length to the length of just the first item.
	// If the broken length is shorter, we will just use that and skip
	// considering additional entries in the array. While not ideal, this
	// will minimise the pain of changing the texture hash so soon after
	// the last time.
	//
	// TODO: We might consider an ini setting to disable this fallback for
	// new games, or possibly to force it for old games.
	length = Texture2DLength(pDesc, &pInitialData[0], 0);
	LogDebug("  Texture2D length: %Iu bad v1.2.1 length: %Iu\n", length, length_v12);
	if (length_v12 <= length) {
		if (length_v12 < length || pDesc->ArraySize > 1) {
			LogDebug("  Using 3DMigoto v1.2.1 compatible Texture2D CRC calculation\n");
		}
		return hash_tex2d_data(hash, pInitialData[0].pSysMem, length_v12,
				pDesc, zero_padding, false, pInitialData[0].SysMemPitch);
	}

	// If we are here it means the old length had overflowed the buffer,
	// which means we could not rely on it being a consistent value unless
	// we got lucky and the memory following the buffer was always
	// consistent (and even if we did, can we be sure every player will,
	// and that it won't change when the game is updated?).
	//
	// In that case, let's do it right... and hopefully this will be the
	// last time we need to change this.

	LogDebug("  Using 3DMigoto v1.2.11+ Texture2D CRC calculation\n");

	// We are no longer taking multiple subresources into account in the
	// hash. We did for a short time between 3DMigoto 1.2.9 and 1.2.10, but
	// then it was discovered that some games are updating resources which
	// made matching their hash impossible without tracking the updates.
	//
	// This complicates matters if a multi-element resource gets an update
	// to only a single subresource. In that case, we would have no way to
	// recalculate the hash from all subresources (well, not unless we pull
	// the other subresources back from the GPU and kill performance) and
	// would not be able to track them.
	//
	// For now, we are solving this dilemma by only using the hash from the
	// first subresource for all subresources in the texture. In the
	// future, we could consider an alternate approach that calculates
	// individual hashes for each subresource and xors them all together
	// for the texture as a whole, thereby allowing each subresource to be
	// tracked individually, but this would be a pretty fundamental change
	// since we would probably want to change all hashes, not just those of
	// multi-element resources - so not something we would do unless
	// necessary.
	//
	// 3DMigoto 1.2.9 already changed the hash of multi-element resources,
	// but none of our fixes were reported broken by that change. Therefore
	// I am fairly confident that there won't be any impact to this change
	// either.
	//
	// This is now fairly ingrained that we only consider the first
	// subresource. Changing this would break hash tracking and frame
	// analysis de-duplication.

	length = Texture2DLength(pDesc, &pInitialData[0], 0);
	hash = hash_tex2d_data(hash, pInitialData[0].pSysMem, length,
			pDesc, false, true, pInitialData[0].SysMemPitch);

	return hash;
}

uint32_t CalcTexture2DDataHashAccurate(
	const D3D11_TEXTURE2D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData)
{
	uint32_t hash = 0;

	// The regular data hash we are using is woefully innaccurate, as it
	// will not hash the entire image. Mostly OK for texture filtering, but
	// no good for frame analysis deduplication - especially evident when
	// the HUD is being rendered, as only HUD elements that alter the upper
	// third of the image cause the hash to change, while HUD elements in
	// the mid to lower half of the image don't affect the hash at all
	// (observed in DOAXVV).
	//
	// This function throws away all backwards compatibility with our
	// legacy hashing code to just try to do it right. The hashes won't
	// match those used for texture filtering at all, but it's more
	// important that this get it right.

	if (!pDesc || !pInitialData || !pInitialData->pSysMem)
		return 0;

	// Passing length=INT_MAX, since that is an upper bound and
	// hash_tex2d_data will work it out from DirectXTK
	hash = hash_tex2d_data(hash, pInitialData[0].pSysMem, INT_MAX,
			pDesc, false, true, pInitialData[0].SysMemPitch);

	return hash;
}

// Must be called with the critical section held to protect mResources against
// simultaneous reads & modifications (hmm, tempted to implement a lock free
// map given that it's add only, or use RCU). Is there anything on Windows like
// lockdep to statically prove this is called with the lock held?
ResourceHandleInfo* GetResourceHandleInfo(ID3D11Resource *resource)
{
	std::unordered_map<ID3D11Resource *, ResourceHandleInfo>::iterator j;
	ResourceHandleInfo* ret = NULL;

	EnterCriticalSectionPretty(&G->mResourcesLock);

	j = lookup_resource_handle_info(resource);
	if (j != G->mResources.end())
		ret = &j->second;

	LeaveCriticalSection(&G->mResourcesLock);

	return ret;
}

// Must be called with the critical section held to protect mResources against
// simultaneous reads & modifications
uint32_t GetOrigResourceHash(ID3D11Resource *resource)
{
	ResourceHandleInfo *handle_info = GetResourceHandleInfo(resource);
	if (handle_info)
		return handle_info->orig_hash;

	return 0;
}

// Must be called with the critical section held to protect mResources against
// simultaneous reads & modifications
uint32_t GetResourceHash(ID3D11Resource *resource)
{
	ResourceHandleInfo *handle_info = GetResourceHandleInfo(resource);
	if (handle_info)
		return handle_info->hash;

	// We can get here for a few legitimate reasons where a resource has
	// not been hashed. Resources created by 3DMigoto bypass the
	// CreateTexture wrapper and are not hashed, and the swap chain's back
	// buffer will not have been hashed. We used to hash these on demand
	// here, but it's not clear that we ever needed their hashes - if we
	// ever do we could consider hashing them at the time of creation
	// instead.
	//
	// Return a 0 so it is obvious that this resource has not been hashed.

	return 0;
}

uint64_t GetResourcePerceptualHash(ID3D11Resource *resource)
{
	ResourceHandleInfo *handle_info = GetResourceHandleInfo(resource);
	if (handle_info && handle_info->perceptual_hash_valid)
		return handle_info->perceptual_hash;
	return 0;
}

uint64_t CalcTexture2DPerceptualHash(
	const D3D11_TEXTURE2D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData)
{
	TexturePHashInfo info;

	if (!compute_texture2d_phash_info(pDesc, pInitialData, &info))
		return 0;

	return info.canonical_hash;
}

uint32_t CalcTexture1DDataHash(
	const D3D11_TEXTURE1D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData)
{
	size_t length;

	if (!pDesc || !pInitialData || !pInitialData->pSysMem)
		return 0;

	length = Texture1DLength(pDesc, &pInitialData[0], 0);
	return crc32c_hw(0, pInitialData[0].pSysMem, length);
}

uint32_t CalcTexture3DDataHash(
	const D3D11_TEXTURE3D_DESC *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData)
{
	uint32_t hash = 0;
	size_t length_v12;
	size_t length;

	if (!pDesc || !pInitialData || !pInitialData->pSysMem)
		return 0;

	// In 3DMigoto v1.2, this is what we were using as the length of the
	// buffer in bytes. Unfortunately this is not right since pDesc->Width
	// is in texels, not bytes. Additionally, compressed textures
	// complicate the buffer size calculation further.
	//
	// The result is that we might not consider the entire buffer when
	// calculating the hash (which may not be ideal, but it is generally
	// acceptable), or we might overflow the buffer. If we overflow we
	// might get an exception if there is nothing mapped after the buffer
	// (which we catch and log), but we could just as easily process
	// gargage after the buffer as being part of the texture, which would
	// lead to us creating unpredictable hashes.
	length_v12 = pDesc->Width * pDesc->Height * pDesc->Depth;

	// Compare the old broken length to the actual length. If the broken
	// length is shorter, we will just use that. While not ideal, this will
	// minimise the pain of changing the texture hash so soon after the
	// last time.
	//
	// TODO: We might consider an ini setting to disable this fallback for
	// new games, or possibly to force it for old games.
	length = Texture3DLength(pDesc, &pInitialData[0], 0);
	LogDebug("  Texture3D length: %Iu bad v1.2.1 length: %Iu\n", length, length_v12);
	if (length_v12 <= length) {
		if (length_v12 < length) {
			LogDebug("  Using 3DMigoto v1.2.1 compatible Texture3D CRC calculation\n");
		}
		return crc32c_hw(hash, pInitialData[0].pSysMem, length_v12);
	}

	// If we are here it means the old length had overflowed the buffer,
	// which means we could not rely on it being a consistent value unless
	// we got lucky and the memory following the buffer was always
	// consistent (and even if we did, can we be sure every player will,
	// and that it won't change when the game is updated?).
	//
	// In that case, let's do it right... and hopefully this will be the
	// last time we need to change this.

	LogDebug("  Using 3DMigoto v1.2.9+ Texture3D CRC calculation\n");

	hash = crc32c_hw(hash, pInitialData[0].pSysMem, length);

	return hash;
}

static bool supports_hash_tracking(ResourceHandleInfo *handle_info)
{
	// We only support hash tracking and contamination detection for 2D and
	// 3D textures currently. We could probably add 1D textures relatively
	// safely, but buffers would kill performance because of how often they
	// are updated, so we're skipping them for now. If we do want to add
	// support for them later, we should add a means to turn off the
	// contamination detection on a per-resource type basis:
	return (handle_info->type == D3D11_RESOURCE_DIMENSION_TEXTURE2D ||
		handle_info->type == D3D11_RESOURCE_DIMENSION_TEXTURE3D);
}

static bool GetResourceInfoFields(struct ResourceHashInfo *info, UINT subresource,
		UINT *width, UINT *height, UINT *depth,
		UINT *idx, UINT *mip, UINT *array_size)
{
	UINT mips;
	switch (info->type) {
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			mips = max(info->tex2d_desc.MipLevels, 1);
			*idx = subresource / mips;
			*mip = subresource % mips;
			*width = max(info->tex2d_desc.Width >> *mip, 1);
			*height = max(info->tex2d_desc.Height >> *mip, 1);
			*depth = 1;
			*array_size = info->tex2d_desc.ArraySize;
			return true;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			mips = max(info->tex3d_desc.MipLevels, 1);
			*idx = subresource / mips;
			*mip = subresource % mips;
			*width = max(info->tex3d_desc.Width >> *mip, 1);
			*height = max(info->tex3d_desc.Height >> *mip, 1);
			*depth = max(info->tex3d_desc.Depth >> *mip, 1);
			*array_size = 1;
			return true;
	}
	return false;
}

void MarkResourceHashContaminated(ID3D11Resource *dest, UINT DstSubresource,
		ID3D11Resource *src, UINT srcSubresource, char type,
		UINT DstX, UINT DstY, UINT DstZ, const D3D11_BOX *SrcBox)
{
	ResourceHandleInfo *dst_handle_info;
	struct ResourceHashInfo *dstInfo, *srcInfo = NULL;
	uint32_t srcHash = 0, dstHash = 0;
	UINT srcWidth = 1, srcHeight = 1, srcDepth = 1, srcMip = 0, srcIdx = 0, srcArraySize = 1;
	UINT dstWidth = 1, dstHeight = 1, dstDepth = 1, dstMip = 0, dstIdx = 0, dstArraySize = 1;
	bool partial = false;
	ResourceInfoMap::iterator info_i;
	Profiling::State profiling_state;

	if (!dest)
		return;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	EnterCriticalSectionPretty(&G->mCriticalSection);

	dst_handle_info = GetResourceHandleInfo(dest);
	if (!dst_handle_info)
		goto out_unlock;

	if (!supports_hash_tracking(dst_handle_info))
		goto out_unlock;

	dstHash = dst_handle_info->orig_hash;
	if (!dstHash)
		goto out_unlock;

	// Faster than catching an out_of_range exception from .at():
	info_i = G->mResourceInfo.find(dstHash);
	if (info_i == G->mResourceInfo.end())
		goto out_unlock;
	dstInfo = &info_i->second;

	GetResourceInfoFields(dstInfo, DstSubresource,
			&dstWidth, &dstHeight, &dstDepth,
			&dstIdx, &dstMip, &dstArraySize);

	// We don't care if a mip-map has been updated since we don't hash those.
	// We could collect info about the copy anyway (below code will work to
	// do so), but it adds a lot of irrelevant noise to the ShaderUsage.txt
	if (dstMip)
		goto out_unlock;

	if (src) {
		srcHash = GetOrigResourceHash(src);
		G->mCopiedResourceInfo.insert(srcHash);

		// Faster than catching an out_of_range exception from .at():
		info_i = G->mResourceInfo.find(srcHash);
		if (info_i != G->mResourceInfo.end()) {
			srcInfo = &info_i->second;
			GetResourceInfoFields(srcInfo, srcSubresource,
					&srcWidth, &srcHeight, &srcDepth,
					&srcIdx, &srcMip, &srcArraySize);

			if (dstHash != srcHash && srcInfo->initial_data_used_in_hash) {
				dstInfo->initial_data_used_in_hash = true;
				if (G->track_texture_updates == 0)
					dstInfo->hash_contaminated = true;
			}
		}
	}

	switch (type) {
		case 'U':
			dstInfo->update_contamination.insert(DstSubresource);
			dstInfo->initial_data_used_in_hash = true;
			if (G->track_texture_updates == 0)
				dstInfo->hash_contaminated = true;
			break;
		case 'M':
			dstInfo->map_contamination.insert(DstSubresource);
			dstInfo->initial_data_used_in_hash = true;
			if (G->track_texture_updates == 0)
				dstInfo->hash_contaminated = true;
			break;
		case 'C':
			dstInfo->copy_contamination.insert(srcHash);
			break;
		case 'S':

			// We especially want to know if a region copy copied
			// the entire texture, or only part of it. This may be
			// important if we end up changing the hash due to a
			// copy operation - if it copied the whole resource, we
			// can just use the hash of the source. If it only
			// copied a partial resource there's no good answer.

			partial = partial || dstWidth != srcWidth;
			partial = partial || dstHeight != srcHeight;
			partial = partial || dstDepth != srcDepth;

			partial = partial || DstX || DstY || DstZ;
			if (SrcBox) {
				partial = partial ||
					(SrcBox->right - SrcBox->left != dstWidth) ||
					(SrcBox->bottom - SrcBox->top != dstHeight) ||
					(SrcBox->back - SrcBox->front != dstDepth);
			}

			// TODO: Need to think about the implications of
			// copying between textures with > 1 array element.
			// Might want to reconsider how these are hashed (e.g.
			// hash each non-mipmap subresource separately and xor
			// the hashes together so we can efficiently change a
			// single subhash)
			partial = partial || dstArraySize > 1 || srcArraySize > 1;

			dstInfo->region_contamination[
					std::make_tuple(srcHash, dstIdx, dstMip, srcIdx, srcMip)
				].Update(partial, DstX, DstY, DstZ, SrcBox);
	}

out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::hash_tracking_overhead);
}

void UpdateResourceHashFromCPU(ID3D11Resource *resource,
	const void *data, UINT rowPitch, UINT depthPitch)
{
	D3D11_RESOURCE_DIMENSION dim;
	D3D11_SUBRESOURCE_DATA initialData;
	ID3D11Texture2D *tex2D;
	ID3D11Texture3D *tex3D;
	D3D11_TEXTURE2D_DESC *desc2D;
	D3D11_TEXTURE3D_DESC *desc3D;
	uint32_t old_data_hash, old_hash;
	ResourceHandleInfo *info = NULL;
	Profiling::State profiling_state;

	if (!resource || !data)
		return;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	EnterCriticalSectionPretty(&G->mCriticalSection);

	info = GetResourceHandleInfo(resource);
	if (!info)
		goto out_unlock;

	if (!supports_hash_tracking(info))
		goto out_unlock;

	// Ever noticed that D3D11_SUBRESOURCE_DATA is binary identical to
	// D3D11_MAPPED_SUBRESOURCE but they changed all the names around?
	initialData.pSysMem = data;
	initialData.SysMemPitch = rowPitch;
	initialData.SysMemSlicePitch = depthPitch;

	// TODO: We currently store the desc structure that was originally used
	// when the resource was created. We can query the desc from the
	// resource directly to save memory, but there are some potential
	// differences between what we stored and what we get from the query.
	// MipMaps may be set to 0 at creation time, which will cause DX to
	// generate them and I presume would then be set when we query the
	// desc. Most of the other fields should be the same, but I'm not
	// positive about all the misc flags. Once we understand all possible
	// differences we could just store those instead of the whole struct.

	old_data_hash = info->data_hash;
	old_hash = info->hash;

	resource->GetType(&dim);
	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			tex2D = (ID3D11Texture2D*)resource;

			desc2D = &info->desc2D;
			// TODO: tex2D->GetDesc(&desc2D); then fix up mip-maps if necessary

			info->data_hash = CalcTexture2DDataHash(desc2D, &initialData);
			info->hash = CalcTexture2DDescHash(info->data_hash, desc2D);
			info->perceptual_hash = CalcTexture2DPerceptualHash(desc2D, &initialData);
			info->perceptual_hash_valid = !!info->perceptual_hash;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			tex3D = (ID3D11Texture3D*)resource;

			desc3D = &info->desc3D;
			// TODO: tex3D->GetDesc(&desc3D); then fix up mip-maps if necessary

			info->data_hash = CalcTexture3DDataHash(desc3D, &initialData);
			info->hash = CalcTexture3DDescHash(info->data_hash, desc3D);
			info->InvalidatePerceptualHash();
			break;
	}

	LogDebug("Updated resource hash\n");
	LogDebug("  old data: %08x new data: %08x\n", old_data_hash, info->data_hash);
	LogDebug("  old hash: %08x new hash: %08x\n", old_hash, info->hash);

out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::hash_tracking_overhead);
}

void UpdateResourcePerceptualHashFromCPU(ID3D11Resource *resource,
	const void *data, UINT rowPitch, UINT depthPitch)
{
	D3D11_SUBRESOURCE_DATA initialData;
	ResourceHandleInfo *info = NULL;

	if (!resource || !data)
		return;

	EnterCriticalSectionPretty(&G->mCriticalSection);

	info = GetResourceHandleInfo(resource);
	if (!info)
		goto out_unlock;

	if (info->type != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		goto out_invalidate;

	initialData.pSysMem = data;
	initialData.SysMemPitch = rowPitch;
	initialData.SysMemSlicePitch = depthPitch;

	info->perceptual_hash = CalcTexture2DPerceptualHash(&info->desc2D, &initialData);
	info->perceptual_hash_valid = !!info->perceptual_hash;
	goto out_unlock;

out_invalidate:
	info->InvalidatePerceptualHash();
out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);
}

void PropagateResourceHash(ID3D11Resource *dst, ID3D11Resource *src)
{
	ResourceHandleInfo *dst_info, *src_info;
	D3D11_RESOURCE_DIMENSION dim;
	D3D11_TEXTURE2D_DESC *desc2D;
	D3D11_TEXTURE3D_DESC *desc3D;
	uint32_t old_data_hash, old_hash;
	Profiling::State profiling_state;

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::start(&profiling_state);

	EnterCriticalSectionPretty(&G->mCriticalSection);

	dst_info = GetResourceHandleInfo(dst);
	if (!dst_info)
		goto out_unlock;

	if (!supports_hash_tracking(dst_info))
		goto out_unlock;

	src_info = GetResourceHandleInfo(src);
	if (!src_info)
		goto out_unlock;

	// If nothing observable changes we can skip the bookkeeping.
	if (src_info->data_hash == dst_info->data_hash
	 && src_info->perceptual_hash_valid == dst_info->perceptual_hash_valid
	 && src_info->perceptual_hash == dst_info->perceptual_hash)
		goto out_unlock;

	// XXX: If the destination had an initial data but the source did not
	// we will currently discard the data part of the hash - is that the
	// best thing to do, or should we leave the destination untouched? I'm
	// going on the assumption that we care more about what the hash
	// represents *now* rather than when it was created and if a texture is
	// being dynamically updated by the GPU it doesn't make sense to use
	// the initial data hash... but I'm not certain that will be the best
	// decision for every game... We could always make it an option in the
	// d3dx.ini if need be...

	old_data_hash = dst_info->data_hash;
	old_hash = dst_info->hash;

	dst_info->data_hash = src_info->data_hash;

	dst->GetType(&dim);
	switch (dim) {
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			desc2D = &dst_info->desc2D;
			// TODO: tex2D->GetDesc(&desc2D); then fix up mip-maps if necessary

			dst_info->hash = CalcTexture2DDescHash(dst_info->data_hash, desc2D);
			if (src_info->perceptual_hash_valid) {
				dst_info->perceptual_hash = src_info->perceptual_hash;
				dst_info->perceptual_hash_valid = true;
			} else {
				dst_info->InvalidatePerceptualHash();
			}
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			desc3D = &dst_info->desc3D;
			// TODO: tex3D->GetDesc(&desc3D); then fix up mip-maps if necessary

			dst_info->hash = CalcTexture3DDescHash(dst_info->data_hash, desc3D);
			dst_info->InvalidatePerceptualHash();
			break;
	}

	LogDebug("Propagated resource hash\n");
	LogDebug("  old data: %08x new data: %08x\n", old_data_hash, dst_info->data_hash);
	LogDebug("  old hash: %08x new hash: %08x\n", old_hash, dst_info->hash);

out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);

	if (Profiling::mode == Profiling::Mode::SUMMARY)
		Profiling::end(&profiling_state, &Profiling::hash_tracking_overhead);
}

void PropagateResourcePerceptualHash(ID3D11Resource *dst, ID3D11Resource *src)
{
	ResourceHandleInfo *dst_info, *src_info;

	EnterCriticalSectionPretty(&G->mCriticalSection);

	dst_info = GetResourceHandleInfo(dst);
	src_info = GetResourceHandleInfo(src);

	if (!dst_info)
		goto out_unlock;
	if (!src_info)
		goto out_invalidate;

	if (dst_info->type != D3D11_RESOURCE_DIMENSION_TEXTURE2D
	 || src_info->type != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		goto out_invalidate;

	if (!src_info->perceptual_hash_valid)
		goto out_invalidate;

	dst_info->perceptual_hash = src_info->perceptual_hash;
	dst_info->perceptual_hash_valid = true;
	goto out_unlock;

out_invalidate:
	dst_info->InvalidatePerceptualHash();
out_unlock:
	LeaveCriticalSection(&G->mCriticalSection);
}

bool MapTrackResourceHashUpdate(ID3D11Resource *pResource, UINT Subresource)
{
	if (G->hunting && G->track_texture_updates != 2) { // Any hunting mode - want to catch hash contamination even while soft disabled
		MarkResourceHashContaminated(pResource, Subresource, NULL, 0, 'M', 0, 0, 0, NULL);
	}

	// TODO: If track_texture_updated is disabled, but we are in hunting
	// with a reloadable config, we might consider tracking the data hash
	// updates regardless (just not the full resource hash) so the option
	// can be turned on live and work. But there's a few pieces we would
	// need for that to work so for now let's not over-complicate things.
	return G->track_texture_updates == 1 && Subresource == 0;
}

// -----------------------------------------------------------------------------------------------
//                       Automatic Data Structure Cleanup on Resource Release
// -----------------------------------------------------------------------------------------------

// {4A40BF2F-6358-470F-BA0A-662E3E2D8CD3}
DEFINE_GUID(ResourceReleaseTrackerGuid,
0x4a40bf2f, 0x6358, 0x470f, 0xba, 0xa, 0x66, 0x2e, 0x3e, 0x2d, 0x8c, 0xd3);

ResourceReleaseTracker::ResourceReleaseTracker(ID3D11Resource *resource) :
	resource(resource)
{
	ref = 0;
	HRESULT hr = resource->SetPrivateDataInterface(ResourceReleaseTrackerGuid, this);
	// LogDebug("ResourceReleaseTracker %p tracking %p: 0x%x\n", this, resource, hr);
}

HRESULT STDMETHODCALLTYPE ResourceReleaseTracker::QueryInterface(REFIID riid, _COM_Outptr_ void **ppvObject)
{
	LogInfo("ResourceReleaseTracker::QueryInterface(%p:%p) called with IID: %s\n", this, resource, NameFromIID(riid).c_str());

	// The only interface we support is IUnknown
	if (ppvObject && IsEqualIID(riid, IID_IUnknown)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ResourceReleaseTracker::AddRef(void)
{
	ULONG ret = ++ref;
	// LogDebug("ResourceReleaseTracker::AddRef(%p:%p) -> %lu\n", this, resource, ret);
	return ret;
}

ULONG STDMETHODCALLTYPE ResourceReleaseTracker::Release(void)
{
	ULONG ret = --ref;
	// LogDebug("ResourceReleaseTracker::Release(%p:%p) -> %lu\n", this, resource, ret);
	if (ret == 0) {
		// LogDebug("Removing %p from mResources\n", resource);

		////////////////////////////////////////////////////////////
		//                                                        //
		//            <==============================>            //
		//            < AB-BA TYPE DEADLOCK WARNING! >            //
		//            <==============================>            //
		//                                                        //
		// DirectX has called us with a lock held, and we are now //
		// taking our critical section to update mResources.      //
		// If we ever call into DirectX with our critical section //
		// held and it tries to take it's lock we have a possible //
		// AB-BA type deadlock scenario!                          //
		//                                                        //
		// We should aim to never call into DirectX with this     //
		// particular lock held. If we ever do need to call into  //
		// DirectX with this lock held, split the lock into two   //
		// finer grained locks so that the mResources lock is not //
		// held while calling DirectX. Be mindful that adding too //
		// many locks without lockdep is risky in and of itself.  //
		//                                                        //
		// Issue uncovered in the Resident Evil 2 remake when the //
		// overlay called into DirectX to draw notices with this  //
		// lock held to protect it's notices data structure.      //
		//                                                        //
		////////////////////////////////////////////////////////////
		EnterCriticalSectionPretty(&G->mResourcesLock);
		G->mResources.erase(resource);
		LeaveCriticalSection(&G->mResourcesLock);
		delete this;
	}
	return ret;
}

// -----------------------------------------------------------------------------------------------
//                       Fuzzy Texture Override Matching Support
// -----------------------------------------------------------------------------------------------

FuzzyMatch::FuzzyMatch()
{
	op = FuzzyMatchOp::ALWAYS;
	rhs_type1 = FuzzyMatchOperandType::VALUE;
	rhs_type2 = FuzzyMatchOperandType::VALUE;
	val = 0;
	mask = 0xffffffff;
	numerator = 1;
	denominator = 1;
}

static UINT get_resource_width(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_width(const D3D11_TEXTURE1D_DESC *desc) { return desc->Width; }
static UINT get_resource_width(const D3D11_TEXTURE2D_DESC *desc) { return desc->Width; }
static UINT get_resource_width(const D3D11_TEXTURE3D_DESC *desc) { return desc->Width; }

static UINT get_resource_height(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_height(const D3D11_TEXTURE1D_DESC *desc) { return 0; }
static UINT get_resource_height(const D3D11_TEXTURE2D_DESC *desc) { return desc->Height; }
static UINT get_resource_height(const D3D11_TEXTURE3D_DESC *desc) { return desc->Height; }

static UINT get_resource_depth(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE1D_DESC *desc) { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE2D_DESC *desc) { return 0; }
static UINT get_resource_depth(const D3D11_TEXTURE3D_DESC *desc) { return desc->Depth; }

static UINT get_resource_array(const D3D11_BUFFER_DESC *desc)    { return 0; }
static UINT get_resource_array(const D3D11_TEXTURE1D_DESC *desc) { return desc->ArraySize; }
static UINT get_resource_array(const D3D11_TEXTURE2D_DESC *desc) { return desc->ArraySize; }
static UINT get_resource_array(const D3D11_TEXTURE3D_DESC *desc) { return 0; }

template <typename DescType>
static UINT eval_field(FuzzyMatchOperandType type, UINT val, const DescType *desc)
{
	switch (type) {
		case FuzzyMatchOperandType::VALUE:
			return val;
		case FuzzyMatchOperandType::WIDTH:
			return get_resource_width(desc);
		case FuzzyMatchOperandType::HEIGHT:
			return get_resource_height(desc);
		case FuzzyMatchOperandType::DEPTH:
			return get_resource_depth(desc);
		case FuzzyMatchOperandType::ARRAY:
			return get_resource_array(desc);
		case FuzzyMatchOperandType::RES_WIDTH:
			return G->mResolutionInfo.width;
		case FuzzyMatchOperandType::RES_HEIGHT:
			return G->mResolutionInfo.height;
	};

	LogOverlay(LOG_DIRE, "BUG: Invalid fuzzy field %u\n", type);

	return val;
}

template <typename DescType>
bool FuzzyMatch::matches(UINT lhs, const DescType *desc) const
{
	UINT effective;

	// Common case:
	if (op == FuzzyMatchOp::ALWAYS)
		return true;

	effective = eval_field(rhs_type1, val, desc);

	// Second named field, for match_byte_width = res_width * res_height in RE7
	effective *= eval_field(rhs_type2, 1, desc);

	return matches_common(lhs, effective);
}

bool FuzzyMatch::matches_uint(UINT lhs) const
{
	// Common case:
	if (op == FuzzyMatchOp::ALWAYS)
		return true;

	if (rhs_type1 != FuzzyMatchOperandType::VALUE)
		return false;

	return matches_common(lhs, val);
}

bool FuzzyMatch::matches_common(UINT lhs, UINT effective) const
{
	// For now just supporting a single integer numerator and denominator,
	// which should be sufficient to match most aspect ratios, downsampled
	// textures and so on. TODO: Add a full expression evaluator.
	if (!denominator)
		return false;
	effective = effective * numerator / denominator;

	switch (op) {
		case FuzzyMatchOp::EQUAL:
			// Only case that the mask applies to, for flags fields
			return ((lhs & mask) == effective);
		case FuzzyMatchOp::LESS:
			return (lhs < effective);
		case FuzzyMatchOp::LESS_EQUAL:
			return (lhs <= effective);
		case FuzzyMatchOp::GREATER:
			return (lhs > effective);
		case FuzzyMatchOp::GREATER_EQUAL:
			return (lhs >= effective);
		case FuzzyMatchOp::NOT_EQUAL:
			return (lhs != effective);
	};

	return false;
}

FuzzyMatchResourceDesc::FuzzyMatchResourceDesc(std::wstring section) :
	matches_buffer(true),
	matches_tex1d(true),
	matches_tex2d(true),
	matches_tex3d(true)
{
	// TODO: Statically contain this once we sort out our header files:
	texture_override = new TextureOverride();
	texture_override->ini_section = section;
}

FuzzyMatchResourceDesc::~FuzzyMatchResourceDesc()
{
	delete texture_override;
}

template <typename DescType>
bool FuzzyMatchResourceDesc::check_common_resource_fields(const DescType *desc) const
{
	if (!Usage.matches(desc->Usage, desc))
		return false;
	if (!BindFlags.matches(desc->BindFlags, desc))
		return false;
	if (!CPUAccessFlags.matches(desc->CPUAccessFlags, desc))
		return false;
	if (!MiscFlags.matches(desc->MiscFlags, desc))
		return false;
	return true;
}

template <typename DescType>
bool FuzzyMatchResourceDesc::check_common_texture_fields(const DescType *desc) const
{
	if (!MipLevels.matches(desc->MipLevels, desc))
		return false;
	if (!Format.matches(desc->Format, desc))
		return false;
	if (!Width.matches(desc->Width, desc))
		return false;
	return true;
}

bool FuzzyMatchResourceDesc::matches(const D3D11_BUFFER_DESC *desc) const
{
	if (!matches_buffer)
		return false;

	if (!check_common_resource_fields(desc))
		return false;

	if (!ByteWidth.matches(desc->ByteWidth, desc))
		return false;
	if (!StructureByteStride.matches(desc->StructureByteStride, desc))
		return false;
	return true;
}

bool FuzzyMatchResourceDesc::matches(const D3D11_TEXTURE1D_DESC *desc) const
{
	if (!matches_tex1d)
		return false;

	if (!check_common_resource_fields(desc))
		return false;
	if (!check_common_texture_fields(desc))
		return false;

	if (!ArraySize.matches(desc->ArraySize, desc))
		return false;
	return true;
}

bool FuzzyMatchResourceDesc::matches(const D3D11_TEXTURE2D_DESC *desc) const
{
	if (!matches_tex2d)
		return false;

	if (!check_common_resource_fields(desc))
		return false;
	if (!check_common_texture_fields(desc))
		return false;

	if (!Height.matches(desc->Height, desc))
		return false;
	if (!ArraySize.matches(desc->ArraySize, desc))
		return false;
	if (!SampleDesc_Count.matches(desc->SampleDesc.Count, desc))
		return false;
	if (!SampleDesc_Quality.matches(desc->SampleDesc.Quality, desc))
		return false;
	return true;
}

bool FuzzyMatchResourceDesc::matches(const D3D11_TEXTURE3D_DESC *desc) const
{
	if (!matches_tex3d)
		return false;

	if (!check_common_resource_fields(desc))
		return false;
	if (!check_common_texture_fields(desc))
		return false;

	if (!Height.matches(desc->Height, desc))
		return false;
	if (!Depth.matches(desc->Depth, desc))
		return false;
	return true;
}

void FuzzyMatchResourceDesc::set_resource_type(D3D11_RESOURCE_DIMENSION type)
{
	switch(type) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			matches_tex1d = matches_tex2d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			matches_buffer = matches_tex2d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			matches_buffer = matches_tex1d = matches_tex3d = false;
			return;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			matches_buffer = matches_tex1d = matches_tex2d = false;
			return;
	}
}

bool FuzzyMatchResourceDesc::update_types_matched()
{
	// This will remove the flags for types of resources we cannot match
	// based on what fields we are matching and what resource types they
	// apply to. We do not set a flag if it was already cleared, since the
	// user may have already specified a specific resource type. If we are
	// left with no possible resource types we can match we will return
	// false so that the caller knows this is invalid.

	if (FuzzyMatchOp::ALWAYS != ByteWidth.op
	 || FuzzyMatchOp::ALWAYS != StructureByteStride.op)
		matches_tex1d = matches_tex2d = matches_tex3d = false;

	if (FuzzyMatchOp::ALWAYS != MipLevels.op
	 || FuzzyMatchOp::ALWAYS != Format.op
	 || FuzzyMatchOp::ALWAYS != Width.op)
		matches_buffer = false;

	if (FuzzyMatchOp::ALWAYS != Height.op)
		matches_buffer = matches_tex1d = false;

	if (FuzzyMatchOp::ALWAYS != Depth.op)
		matches_buffer = matches_tex1d = matches_tex2d = false;

	if (FuzzyMatchOp::ALWAYS != ArraySize.op)
		matches_buffer = matches_tex3d = false;

	if (FuzzyMatchOp::ALWAYS != SampleDesc_Count.op
	 || FuzzyMatchOp::ALWAYS != SampleDesc_Quality.op)
		matches_buffer = matches_tex1d = matches_tex3d = false;

	return matches_buffer || matches_tex1d || matches_tex2d || matches_tex3d;
}

static bool matches_draw_info(TextureOverride *tex_override, DrawCallInfo *call_info)
{
	if (!tex_override->has_draw_context_match)
		return true;

	if (!call_info)
		return false;

	if (!tex_override->match_index_count.matches_uint(call_info->IndexCount))
		return false;
	if (!tex_override->match_first_index.matches_uint(call_info->FirstIndex))
		return false;
	if (!tex_override->match_vertex_count.matches_uint(call_info->VertexCount))
		return false;
	if (!tex_override->match_first_vertex.matches_uint(call_info->FirstVertex))
		return false;
	if (!tex_override->match_instance_count.matches_uint(call_info->InstanceCount))
		return false;
	if (!tex_override->match_first_instance.matches_uint(call_info->FirstInstance))
		return false;

	return true;
}

void find_texture_override_for_hash(uint32_t hash, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	TextureOverrideMap::iterator i;
	TextureOverrideList::iterator j;

	i = lookup_textureoverride(hash);
	if (i == G->mTextureOverrideMap.end())
		return;

	for (j = i->second.begin(); j != i->second.end(); j++) {
		if (matches_draw_info(&(*j), call_info))
			matches->push_back(&(*j));
	}
}

void find_texture_override_for_phash(uint64_t phash, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	TextureOverridePHashMap::iterator i;
	TextureOverrideList::iterator j;

	i = lookup_textureoverride_phash(phash);
	if (i == G->mTextureOverridePHashMap.end())
		return;

	for (j = i->second.begin(); j != i->second.end(); j++) {
		if (matches_draw_info(&(*j), call_info))
			matches->push_back(&(*j));
	}
}

static uint32_t get_hash_for_resource(ID3D11Resource* resource)
{
	if (!resource)
		return 0;

	EnterCriticalSectionPretty(&G->mCriticalSection);
	uint32_t hash = GetResourceHash(resource);
	LeaveCriticalSection(&G->mCriticalSection);

	return hash;
}

void find_texture_overrides_for_resource_by_hash(ID3D11Resource *resource, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	if (G->mTextureOverrideMap.empty())
		return;

	uint32_t hash = get_hash_for_resource(resource);
	if (!hash)
		return;

	find_texture_override_for_hash(hash, matches, call_info);
}

void find_texture_overrides_for_resource_by_phash(ID3D11Resource *resource, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	uint64_t phash;

	if (G->mTextureOverridePHashMap.empty())
		return;

	phash = GetResourcePerceptualHash(resource);
	if (!phash)
		return;

	find_texture_override_for_phash(phash, matches, call_info);
}

TextureOverrideFuzzyMatches* get_fuzzy_matches_by_draw_info(DrawCallInfo* call_info)
{
	if (call_info->IndexCount)
	{
		auto it = G->mTextureOverrideDrawIndexMap.find(call_info->IndexCount);
		if (it != G->mTextureOverrideDrawIndexMap.end()) {
			return &it->second;
		}
	}
	else if (call_info->VertexCount)
	{
		auto it = G->mTextureOverrideDrawVertexMap.find(call_info->VertexCount);
		if (it != G->mTextureOverrideDrawVertexMap.end()){
			return &it->second;
		}
	}
	return nullptr;
}

void find_texture_overrides_by_hash_from_fuzzy_matches(uint32_t hash, TextureOverrideFuzzyMatches* fuzzy_matches, TextureOverrideMatches* matches, DrawCallInfo* call_info)
{
	TextureOverrideFuzzyMatches::iterator it;

	for (it = fuzzy_matches->begin(); it != fuzzy_matches->end(); ++it) {
		if (it->hash == hash && matches_draw_info(it->texture_override, call_info)) {
			matches->push_back(it->texture_override);
		}
	}
}

void find_texture_overrides_for_resource_by_hash_from_fuzzy_matches(ID3D11Resource* resource, TextureOverrideFuzzyMatches* fuzzy_matches, TextureOverrideMatches* matches, DrawCallInfo* call_info)
{
	uint32_t hash = get_hash_for_resource(resource);
	if (!hash)
		return;

	find_texture_overrides_by_hash_from_fuzzy_matches(hash, fuzzy_matches, matches, call_info);
}

template <typename DescType>
static void find_texture_overrides_for_desc(const DescType *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	FuzzyTextureOverrides::iterator i;

	for (i = G->mFuzzyTextureOverrides.begin(); i != G->mFuzzyTextureOverrides.end(); i++) {
		if ((*i)->matches(desc) && matches_draw_info((*i)->texture_override, call_info))
			matches->push_back((*i)->texture_override);
	}
}

template <typename DescType>
void find_texture_overrides(uint32_t hash, uint64_t phash, const DescType *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	find_texture_override_for_hash(hash, matches, call_info);
	if (!matches->empty()) {
		// If we got a result it was matched by hash - that's an exact
		// match and we don't process any fuzzy matches
		return;
	}

	find_texture_override_for_phash(phash, matches, call_info);
	if (!matches->empty()) {
		// phash is also treated as an exact content match, so only fall
		// back to fuzzy rules if both exact indices missed.
		return;
	}

	find_texture_overrides_for_desc(desc, matches, call_info);
}
// Explicit template expansion is necessary to generate these functions for
// the compiler to generate them so they can be used from other source files:
template void find_texture_overrides<D3D11_BUFFER_DESC>(uint32_t hash, uint64_t phash, const D3D11_BUFFER_DESC *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info);
template void find_texture_overrides<D3D11_TEXTURE1D_DESC>(uint32_t hash, uint64_t phash, const D3D11_TEXTURE1D_DESC *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info);
template void find_texture_overrides<D3D11_TEXTURE2D_DESC>(uint32_t hash, uint64_t phash, const D3D11_TEXTURE2D_DESC *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info);
template void find_texture_overrides<D3D11_TEXTURE3D_DESC>(uint32_t hash, uint64_t phash, const D3D11_TEXTURE3D_DESC *desc, TextureOverrideMatches *matches, DrawCallInfo *call_info);

void find_texture_overrides_for_resource_desc(ID3D11Resource* resource, TextureOverrideMatches* matches, DrawCallInfo* call_info)
{
	D3D11_RESOURCE_DIMENSION dimension;
	resource->GetType(&dimension);
	switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
		{
			ID3D11Buffer* buf = (ID3D11Buffer*)resource;
			D3D11_BUFFER_DESC buf_desc;
			buf->GetDesc(&buf_desc);
			return find_texture_overrides_for_desc(&buf_desc, matches, call_info);
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
		{
			ID3D11Texture1D* tex1d = (ID3D11Texture1D*)resource;
			D3D11_TEXTURE1D_DESC tex1d_desc;
			tex1d->GetDesc(&tex1d_desc);
			return find_texture_overrides_for_desc(&tex1d_desc, matches, call_info);
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
		{
			ID3D11Texture2D* tex2d = (ID3D11Texture2D*)resource;
			D3D11_TEXTURE2D_DESC tex2d_desc;
			tex2d->GetDesc(&tex2d_desc);
			return find_texture_overrides_for_desc(&tex2d_desc, matches, call_info);
		}
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
		{
			ID3D11Texture3D* tex3d = (ID3D11Texture3D*)resource;
			D3D11_TEXTURE3D_DESC tex3d_desc;
			tex3d->GetDesc(&tex3d_desc);
			return find_texture_overrides_for_desc(&tex3d_desc, matches, call_info);
		}
	}
}

void find_texture_overrides_for_resource(ID3D11Resource *resource, TextureOverrideMatches *matches, DrawCallInfo *call_info)
{
	find_texture_overrides_for_resource_by_hash(resource, matches, call_info);
	find_texture_overrides_for_resource_by_phash(resource, matches, call_info);

	// Allow fuzzy matches to be processed even when exact matches exist
	//if (!matches->empty()) {
	//	// If we got a result it was matched by hash - that's an exact
	//	// match and we don't process any fuzzy matches
	//	return;
	//}

	find_texture_overrides_for_resource_desc(resource, matches, call_info);
}

bool TextureOverrideLess(const struct TextureOverride &lhs, const struct TextureOverride &rhs)
{
	// For texture create time overrides we want the highest priority
	// texture override to take precedence, which will happen if it is
	// processed last. Same goes for texture filtering. If the priorities
	// are equal, we use the ini section name for sorting to make sure that
	// we get consistent results.

	if (lhs.priority != rhs.priority)
		return lhs.priority < rhs.priority;
	return lhs.ini_section < rhs.ini_section;
}

bool FuzzyMatchResourceDescLess::operator() (const std::shared_ptr<FuzzyMatchResourceDesc> &lhs, const std::shared_ptr<FuzzyMatchResourceDesc> &rhs) const
{
	return TextureOverrideLess(*lhs->texture_override, *rhs->texture_override);
}

// Initialize page versioning used for fast invalidation.
// Each page tracks a monotonically increasing "version" that invalidates
// all cached entries (offsets) that belong to that page.
// NOTE: Pages do NOT store hashes; they only invalidate offset-based entries.
void RegionHashesCache::Initialize(size_t buffer_size)
{
	//LogInfo("RegionHashesCache::Initialize buffer_size=%d \n", buffer_size);
	UINT num_pages = (UINT)((buffer_size + PAGE_SIZE - 1) / PAGE_SIZE);
	page_versions.assign(num_pages, 0);
	if (cache)
		cache->clear();
}

// Store hash together with the current page version.
// This allows fast invalidation by comparing stored version vs current page version.
void RegionHashesCache::Add(const RegionHashKeyL2& key, uint32_t hash)
{
	if (!cache)
		cache = std::make_unique<FlatHashMap<RegionHashKeyL2, RegionCacheEntry, RegionHashKeyHasherL2>>(page_versions.size() / (PAGE_SIZE / HASHES_PER_PAGE));

	// Compute page index for this offset.
	UINT page = key.offset / PAGE_SIZE;
	if (page >= page_versions.size())
		return;

	RegionCacheEntry entry;
	entry.hash = hash;
	entry.version = page_versions[page];

	cache->insert(key, entry);
}

uint32_t RegionHashesCache::Get(const RegionHashKeyL2& key)
{
	if (!cache)
		return 0;

	UINT page = key.offset / PAGE_SIZE;
	if (page >= page_versions.size())
		return 0;

	// Lookup exact offset (hot path, performance critical).
	const RegionCacheEntry* entry = cache->find_ptr(key);
	if (!entry)
		return 0;

	// Validate against page version
	// If page version changed, this entry is stale.
	if (entry->version != page_versions[page])
		return 0;

	return entry->hash;
}

size_t RegionHashesCache::GetSize()
{
	return cache ? cache->size() : 0;
}

// Invalidate a byte range by bumping page versions.
// This avoids iterating over cache entries.
void RegionHashesCache::Invalidate(UINT start, UINT end)
{
	if (page_versions.empty())
		return;

	UINT start_page = start / PAGE_SIZE;
	if (start_page >= page_versions.size())
		return;

	UINT end_page = (end - 1) / PAGE_SIZE;
	end_page = min(end_page, page_versions.size() - 1);

	if (start_page > end_page)
		return;

	// Incrementing version invalidates ALL entries mapped to that page.
	// This is O(pages), not O(entries), very important for performance.
	for (UINT p = start_page; p <= end_page; ++p) {
		++page_versions[p];
	}

	//LogInfo("RegionHashesCache::Invalidate start=%d, end=%d, start_page=%d, end_page=%d\n", start, end, start_page, end_page);
}

// Full reset of cache and versioning.
// Used when buffer contents are fully replaced or invalid.
void RegionHashesCache::Clear()
{
	//LogInfo("RegionHashesCache::Clear\n");
	if (cache)
		cache->clear();
	// Reset all versions so existing entries (if any reused) become invalid.
	std::fill(page_versions.begin(), page_versions.end(), 0);
}

// Initializes CPU-side snapshot buffer.
// This buffer allows hashing without repeated GPU Map() calls.
void ResourceHandleInfo::InitializeDataCache(size_t size)
{
	// Initialize region hashes cache.
	if (!region_hashes_cache)
		region_hashes_cache = std::make_unique<RegionHashesCache>();
	//LogInfo("InitializeDataCache size=%d\n", size);
	if (!cached_data_size) {
		// First-time initialization.
		cached_data_size = size;
		// Initialize region cache for this buffer size.
		region_hashes_cache->Initialize(size);
	} else {
		// Buffer reused: invalidate all region hashes.
		region_hashes_cache->Clear();
	}
}

void ResourceHandleInfo::WriteDataCache(const void* src, size_t size)
{
	if (!src)
		return;

	//LogInfo("WriteDataCache size=%d\n", size);

	InitializeDataCache(size);

	if (cached_data) {
		free(cached_data);
		cached_data = nullptr;
	}

	// Full overwrite of CPU snapshot.
	cached_data = (uint8_t*)src;

	//info->cached_data_hash = crc32c_hw(0, info->cached_data, size);
}

void ResourceHandleInfo::WriteDataCacheRegion(const void* src, size_t region_size, UINT offset)
{
	if (!src || !region_size)
		return;

	// Cannot write partial region if cache not initialized.
	if (!cached_data_size) {
		LogInfo("WriteDataCacheRegion Failed (not initialized): offset=%d, region_size=%d!\n", offset, region_size);
		return;
	}

	if (offset > cached_data_size || region_size > cached_data_size - offset){
		LogInfo("WriteDataCacheRegion Failed (out of bounds): offset=%d, region_size=%d, dst_size=%d!\n", offset, region_size, cached_data_size);
		return;
	}

	//LogInfo("WriteDataCacheRegion: offset=%d, region_size=%d!\n", offset, region_size);

	if (!cached_data)
		cached_data = (uint8_t*)malloc(cached_data_size);

	if (cached_data) {
	// Update only the affected region.
		memcpy(cached_data + offset, src, region_size);
	}

	// Invalidate only affected pages (cheap, avoids clearing the whole cache).
	if (region_hashes_cache)
		region_hashes_cache->Invalidate(offset, offset + (UINT)region_size);
}

// Clears all cached region hashes and invalidates the CPU-side buffer snapshot.
// This forces region hashes to be recomputed the next time they are requested.
void ResourceHandleInfo::ClearDataCache()
{
	if (!cached_data_size)
		return;

	//LogInfo("ResourceHandleInfo::ClearDataCache\n");

	if (cached_data) {
		free(cached_data);
		cached_data = nullptr;
	}
	cached_data_size = 0;

	// Drop all cached hashes and CPU snapshot.
	if (region_hashes_cache)
		region_hashes_cache->Clear();
}

void ResourceHandleInfo::CacheRegionHash(const RegionHashKeyL2& key, uint32_t hash)
{
	if (region_hashes_cache)
		region_hashes_cache->Add(key, hash);
}

uint32_t ResourceHandleInfo::GetCachedRegionHash(const RegionHashKeyL2& key)
{
	if (!region_hashes_cache)
		return 0;
	return region_hashes_cache->Get(key);
}

void ResourceHandleInfo::InvalidatePerceptualHash()
{
	perceptual_hash = 0;
	perceptual_hash_valid = false;
}

// Helper function that clears region hash cache for a specific D3D resource.
// Used when the underlying resource contents may have changed.
void ClearResourceRegionHashCache(ID3D11Resource* resource)
{
	EnterCriticalSectionPretty(&G->mCriticalSection);
	ResourceHandleInfo* info = GetResourceHandleInfo(resource);
	if (!info) {
		LeaveCriticalSection(&G->mCriticalSection);
		return;
	}
	info->ClearDataCache();
	LeaveCriticalSection(&G->mCriticalSection);
}

void InvalidateResourcePerceptualHash(ID3D11Resource *resource)
{
	EnterCriticalSectionPretty(&G->mCriticalSection);
	ResourceHandleInfo *info = GetResourceHandleInfo(resource);
	if (info)
		info->InvalidatePerceptualHash();
	LeaveCriticalSection(&G->mCriticalSection);
}

// Creates a CPU-readable snapshot of the buffer contents and stores it
// in handle_info->cached_data. The snapshot is taken through a staging
// resource so the GPU buffer can be safely read by the CPU.
static bool CacheBufferData(ID3D11DeviceContext* context, ID3D11Buffer* buffer, ResourceHandleInfo* handle_info)
{
	if (!context || !buffer || !handle_info)
		return false;

	// Fast path: reuse existing CPU snapshot.
	// Avoids expensive GPU sync (CopyResource + Map).
	EnterCriticalSectionPretty(&G->mCriticalSection);
	if (handle_info->cached_data_size) {
		LeaveCriticalSection(&G->mCriticalSection);
		return true;
	}
	LeaveCriticalSection(&G->mCriticalSection);

	// WARNING: Everything below may cause GPU/CPU sync and stall.
	// This is the slow path and should be rare.

	ID3D11Device* dev = NULL;
	context->GetDevice(&dev);
	if (!dev)
		return false;

	// Query the buffer description so we know its size and properties.
	D3D11_BUFFER_DESC desc;
	buffer->GetDesc(&desc);

	// Create a staging buffer with CPU read access.
	// This allows copying GPU memory into a CPU-readable resource.
	D3D11_BUFFER_DESC stagingDesc = desc;
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.BindFlags = 0;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	stagingDesc.MiscFlags = 0;

	ID3D11Buffer* staging = NULL;
	LockResourceCreationMode();
	HRESULT hr = dev->CreateBuffer(&stagingDesc, NULL, &staging);
	UnlockResourceCreationMode();
	if (FAILED(hr)) {
		dev->Release();
		return false;
	}

	// Copy the original GPU buffer contents into the staging buffer.
	context->CopyResource(staging, buffer);

	D3D11_MAPPED_SUBRESOURCE mapped;

	// Map the staging buffer so the CPU can read its contents.
	hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		staging->Release();
		dev->Release();
		return false;
	}

	// Store a CPU copy of the entire buffer so region hashes can be
	// computed without re-mapping the resource multiple times.
	EnterCriticalSectionPretty(&G->mCriticalSection);
	handle_info->WriteDataCache(mapped.pData, desc.ByteWidth);
	LeaveCriticalSection(&G->mCriticalSection);

	context->Unmap(staging, 0);
	staging->Release();
	dev->Release();

	//LogInfo("Fallback CacheBufferData size=%d, hash=%08lx, data_hash=%08lx, pResource=0x%p\n", desc.ByteWidth, handle_info->hash, handle_info->cached_data_hash, buffer);

	return true;
}

UINT GetVertexBufferRegionOffset(UINT stride, DrawCallInfo* call_info, UINT byte_offset)
{
	UINT byte_size = stride * call_info->FirstVertex;
	return byte_offset + byte_size;
}

UINT GetIndexBufferRegionOffset(DXGI_FORMAT format, DrawCallInfo* call_info, UINT byte_offset)
{
	UINT index_stride = (format == DXGI_FORMAT_R32_UINT) ? 4 : 2;
	UINT byte_size = index_stride * call_info->FirstIndex;
	return byte_offset + byte_size;
}

// Computes the byte size of the vertex buffer region used by a draw call.
// Used to determine how much data should be hashed for change detection.
UINT GetVertexBufferRegionSize(UINT stride, DrawCallInfo* call_info)
{
	// If VertexCount is not provided, estimate it from the index count.
	// 0.15 * x + 3
	UINT vertex_count = call_info->VertexCount > 0 ? call_info->VertexCount : (3 * call_info->IndexCount + 10) / 20 + 3;
	UINT region_size = stride * vertex_count;
	//LogInfo("GetVertexBufferRegionSize region_size=%d, stride=%d, VertexCount=%d, IndexCount=%d \n", region_size, stride, call_info->VertexCount, call_info->IndexCount);
	return region_size;
}

// Computes the byte size of the index buffer region referenced by a draw call.
UINT GetIndexBufferRegionSize(DXGI_FORMAT format, DrawCallInfo* call_info)
{
	UINT index_stride = (format == DXGI_FORMAT_R32_UINT) ? 4 : 2;
	UINT region_size = index_stride * call_info->IndexCount;
	//LogInfo("GetIndexBufferRegionSize region_size=%d, stride=%d, IndexCount=%d \n", region_size, index_stride, call_info->IndexCount);
	return region_size;
}

// Global "L3" cache with per-frame reset in HackerSwapChain::Present.
// Optimized for single global "entry point" into TextureOverride's, e.g. `CheckTextureOverride = ib` from global ShaderRegEx.
// Usually, total number of handles is 5-10 times bigger than of ones bound to some specific slot.
// So lookup in dedicated continuous container is expected to be always faster than one in huge unordered map. 
FlatHashMap<RegionHashKeyL3, uint32_t, RegionHashKeyHasherL3> region_hashes_global_cache(1024);

void ClearRegionHashesGlobalCache()
{
	region_hashes_global_cache.clear();
}

// Returns a CRC32 hash for a specific region of the buffer.
// The hash is cached per offset to avoid recomputing it for repeated draw calls.
uint32_t GetRegionHash(ID3D11DeviceContext* context, ID3D11Buffer* buffer, UINT offset, UINT size)
{
	if (!buffer || !size) {
		return 0;
	}

	// Lookup offset in fast L3 cache without any locking involved.
	RegionHashKeyL3 level_3_cache_key{ (uint64_t)buffer, offset, size };
	if (uint32_t* h = region_hashes_global_cache.find_ptr(level_3_cache_key))
	{
		//LogInfo("GetRegionHash: From L3 cache: hash=%08lx, offset=%d, size=%d, pResource=0x%p, cache_size=%d \n", *h, offset, size, buffer, region_hashes_global_cache.size());
		return *h;
	}

	EnterCriticalSectionPretty(&G->mCriticalSection);

	// Acquire HandleInfo. For dozens of thousands of handles in unordered_map, usually it's more expensive than L3 cache lookup. 
	ResourceHandleInfo* handle_info = GetResourceHandleInfo(buffer);
	if (!handle_info) {
		LeaveCriticalSection(&G->mCriticalSection);
		return 0;
	}

	uint32_t hash;

	// Lookup offset in L2 cache. This one is slower and requires `handle_info` lookup.
	RegionHashKeyL2 level_2_cache_key{ (uint64_t)offset, size };
	hash = handle_info->GetCachedRegionHash(level_2_cache_key);
	if (hash) {
		region_hashes_global_cache.insert(level_3_cache_key, hash);
		LeaveCriticalSection(&G->mCriticalSection);
		//LogInfo("GetRegionHash: From L2 cache: hash=%08lx, offset=%d, size=%d, full_hash=%08lx, pResource=0x%p, cache_size=%d \n", hash, offset, size, handle_info->hash, buffer, handle_info->region_hashes_cache->GetSize());
		return hash;
	}

	LeaveCriticalSection(&G->mCriticalSection);

	// Ensure buffer snapshot exists in RAM (will stall GPU to fetch it from VRAM otherwise).
	if (!CacheBufferData(context, buffer, handle_info)) {
		return 0;
	}

	// Pointer to the start of the requested region within the cached buffer.
	if (offset > handle_info->cached_data_size || size > handle_info->cached_data_size - offset) {
		return 0;
	}

	// Make pointer for given offset in L1 cache (raw data).
	const uint8_t* ptr = handle_info->cached_data + offset;

	// Compute CRC32 hash for the region.
	hash = crc32c_hw(0, ptr, size);

	EnterCriticalSectionPretty(&G->mCriticalSection);

	// Store computed region hash in the L2 cache (local per ResourceHandleInfo).
	handle_info->CacheRegionHash(level_2_cache_key, hash);
	// Store computed region hash in the L3 cache (global per-frame).
	region_hashes_global_cache.insert(level_3_cache_key, hash);

	LeaveCriticalSection(&G->mCriticalSection);

	//LogInfo("GetRegionHash: New hash: frame=%d, hash=%08lx, offset=%d, size=%d, full_hash=%08lx, pResource=0x%p, cache_size=%d, data_hash=%08lx \n", G->frame_no, hash, offset, size, handle_info->hash, buffer, handle_info->region_hashes_cache->GetSize(), handle_info->cached_data_hash);

	return hash;
}
