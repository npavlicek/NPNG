#pragma once

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "zlib.h"

namespace PNG
{
using namespace std;

enum class Error
{
	NONE,
	FAILED_TO_OPEN_FILE,
	INVALID_PNG_SIGNATURE,
	DUPLICATE_BLOCK,
	CRC_MISMATCH,
	UNSUPPORTED_IMAGE_OPTS,
	INVALID_BLOCK,
	ZLIB_ERROR,
	INVALID_FILTER_TYPE
};

class Image
{
	vector<unsigned char> raw_data;
	unsigned int cur_pos = 0;
	Error err = Error::NONE;
	bool processed = false;
	bool header_processed = false;
	bool first_block = true;
	vector<unsigned char> compressed_data;
	vector<unsigned char> uncompressed_data;
	string filePath;

	// This is the final data after uncompressing and unfiltering
	vector<unsigned char> data;

	unsigned long width, height;
	unsigned char bit_depth, color_type, compression_method, filter_method, interlace_method, components;

	void process_chunk();
	char paeth_predictor(unsigned char a, unsigned char b, unsigned char c);

  public:
	Image(string filePath);
	Error get_error()
	{
		return err;
	}
	unsigned long get_width()
	{
		return width;
	}
	unsigned long get_height()
	{
		return height;
	}
	unsigned long get_components()
	{
		return components;
	}
	const unsigned char *get_data()
	{
		return data.data();
	}
	unsigned int get_u_int(int idx);
};

#ifdef NPNG_IMPLEMENTATION
unsigned int Image::get_u_int(int idx)
{
	unsigned int res;
	for (int i = 0; i < 4; i++)
	{
		res = (res << 8) | raw_data[idx + i];
	}
	return res;
}

Image::Image(string filePath)
{
	this->filePath = filePath;

	fstream file{filePath, ios::in};

	if (!file.is_open())
	{
		err = Error::FAILED_TO_OPEN_FILE;
		return;
	}

	while (file.peek() != EOF)
	{
		char ch;
		file.get(ch);
		raw_data.push_back(ch);
	}

	file.close();

	// Check png signature
	bool is_png = raw_data[0] == u'\x89' && raw_data[1] == u'\x50' && raw_data[2] == u'\x4E' &&
	              raw_data[3] == u'\x47' && raw_data[4] == u'\x0D' && raw_data[5] == u'\x0A' &&
	              raw_data[6] == u'\x1A' && raw_data[7] == u'\x0A';

	if (!is_png)
	{
		err = Error::INVALID_PNG_SIGNATURE;
		return;
	}

	cur_pos = 8;

	// Process all of the chunks
	while (processed != true)
	{
		process_chunk();
		if (err != Error::NONE)
			return;
	}

	// Now decompress the compressed data
	const int buffer_len = 2048;
	unsigned char buffer[buffer_len];

	z_stream z_str;
	z_str.next_in = compressed_data.data();
	z_str.avail_in = compressed_data.size();
	z_str.next_out = &buffer[0];
	z_str.avail_out = buffer_len;
	z_str.data_type = Z_BINARY;

	z_str.zalloc = Z_NULL;
	z_str.zfree = Z_NULL;
	z_str.opaque = Z_NULL;

	int z_res = inflateInit(&z_str);
	if (z_res != Z_OK)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	do
	{
		z_res = inflate(&z_str, Z_BLOCK);

		if (z_res != Z_OK && z_res != Z_STREAM_END)
		{
			err = Error::ZLIB_ERROR;
			return;
		}

		uncompressed_data.insert(uncompressed_data.end(), &buffer[0], &buffer[buffer_len - z_str.avail_out]);

		z_str.next_out = &buffer[0];
		z_str.avail_out = buffer_len;
	} while (z_res != Z_STREAM_END);

	z_res = inflateEnd(&z_str);
	if (z_res != Z_OK)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	// reverse the filters on our data
	unsigned char *filter_modes = new unsigned char[height];
	int scanline_width = width * components + 1;
	for (int scanline_idx = 0; scanline_idx < height; scanline_idx++)
	{
		filter_modes[scanline_idx] = uncompressed_data[scanline_idx * scanline_width];
		for (int byte_idx = 0; byte_idx < scanline_width; byte_idx++)
		{
			if (byte_idx != 0)
			{
				data.push_back(uncompressed_data[scanline_idx * scanline_width + byte_idx]);
			}
		}
	}

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width * components; x++)
		{
			unsigned char a, b, c;
			unsigned int cur_byte_idx = y * width * components + x;
			unsigned char cur_byte = data[cur_byte_idx];
			if (y == 0 && x < components)
			{
				a = 0;
				b = 0;
				c = 0;
			}
			else if (y == 0)
			{
				a = data[y * width * components + x - components];
				b = 0;
				c = 0;
			}
			else if (x < components)
			{
				a = 0;
				b = data[(y - 1) * width * components + x];
				c = 0;
			}
			else
			{
				a = data[y * width * components + x - components];
				b = data[(y - 1) * width * components + x];
				c = data[(y - 1) * width * components + x - components];
			}

			switch (filter_modes[y])
			{
			case 0:
				continue;
			case 1:
				data[cur_byte_idx] += a;
				data[cur_byte_idx] %= 256;
				continue;
			case 2:
				data[cur_byte_idx] += b;
				data[cur_byte_idx] %= 256;
				continue;
			case 3:
				data[cur_byte_idx] += (a + b) / 2;
				data[cur_byte_idx] %= 256;
				continue;
			case 4:
				data[cur_byte_idx] += paeth_predictor(a, b, c);
				data[cur_byte_idx] %= 256;
				continue;
			case 5:
				err = Error::INVALID_FILTER_TYPE;
				return;
			}
		}
	}
}

char Image::paeth_predictor(unsigned char a, unsigned char b, unsigned char c)
{
	int p = a + b - c;
	int pa = abs(p - a);
	int pb = abs(p - b);
	int pc = abs(p - c);
	if (pa <= pb && pa <= pc)
		return a;
	else if (pb <= pc)
		return b;
	else
		return c;
}

void Image::process_chunk()
{
	unsigned int chunk_len = get_u_int(cur_pos);

	// Add 4 for the chunk len
	cur_pos += 4;

	string chunk_type{&raw_data[cur_pos], &raw_data[cur_pos + 4]};

	unsigned long crc = crc32(0L, raw_data.data() + cur_pos, chunk_len + 4);

	// Add 4 for the chunk_type
	cur_pos += 4;

	unsigned long orig_crc = get_u_int(cur_pos + chunk_len);

	if (crc != orig_crc)
	{
		err = Error::CRC_MISMATCH;
		return;
	}

	// IHDR must be first after PNG sig
	if (first_block && chunk_type != "IHDR")
	{
		err = Error::INVALID_BLOCK;
		return;
	}
	else
	{
		first_block = false;
	}

	if (chunk_type == "IEND")
	{
		processed = true;
	}
	else if (chunk_type == "IDAT")
	{
		compressed_data.insert(compressed_data.end(), &raw_data[cur_pos], &raw_data[cur_pos + chunk_len]);
	}
	else if (chunk_type == "IHDR")
	{
		if (!header_processed)
		{
			width = get_u_int(cur_pos);
			height = get_u_int(cur_pos + 4);
			bit_depth = raw_data[cur_pos + 8];
			color_type = raw_data[cur_pos + 9];
			compression_method = raw_data[cur_pos + 10];
			filter_method = raw_data[cur_pos + 11];
			interlace_method = raw_data[cur_pos + 12];

			if (color_type == 2)
				components = 3;
			else if (color_type == 6)
				components = 4;

			bool invalid_opts = false;

			if (color_type != 2 && color_type != 6)
				invalid_opts = true;

			// We don't support images with a 16 bit width
			if (bit_depth != 8)
				invalid_opts = true;

			if (compression_method != 0)
				invalid_opts = true;

			if (filter_method != 0)
				invalid_opts = true;

			if (interlace_method != 0)
				invalid_opts = true;

			if (invalid_opts)
			{
				err = Error::UNSUPPORTED_IMAGE_OPTS;
				return;
			}

			header_processed = true;
		}
		else
		{
			err = Error::DUPLICATE_BLOCK;
			return;
		}
	}

	// Add 4 for the last 4 crc bytes
	cur_pos += chunk_len + 4;
}
#endif
} // namespace PNG
