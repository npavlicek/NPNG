#pragma once

#include "zlib.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

struct Node
{
	unsigned short data = 0;
	Node *left = nullptr;
	Node *right = nullptr;
	bool is_code = false;
};

class ZStream
{
	Error err = Error::NONE;
	// Decompressed data
	vector<unsigned char> data;
	// Compressed data
	vector<unsigned char> stream;
	unsigned int cur_byte = 0;
	unsigned char cur_bit = 0;
	bool done = false;

	constexpr static unsigned char code_lengths_alphabet[] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
	                                                          11, 4,  12, 3, 13, 2, 14, 1, 15};

	constexpr static unsigned short len_base_alphabet[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
	                                                       15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
	                                                       67, 83, 99, 115, 131, 163, 195, 227, 258};

	constexpr static unsigned char len_extra_bits[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
	                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

	constexpr static unsigned int dist_base_alphabet[] = {
		1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
		193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};

	constexpr static unsigned char dist_extra_bits[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
	                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

	void process_block();
	vector<unsigned short> scan_tree(unsigned short num_codes, Node *code_tree);
	unsigned short scan_code(const Node *tree);
	void decompress_block(const Node *len_tree, const Node *dist_tree);
	void inc_bit()
	{
		cur_bit++;
		cur_byte += cur_bit / 8;
		cur_bit %= 8;
	}
	void inc_bits(unsigned char num)
	{
		unsigned char bytes = num / 8;
		unsigned char bits = num % 8;

		cur_byte += bytes;
		cur_bit += bits;

		cur_byte += cur_bit / 8;
		cur_bit %= 8;
	}

	// Limit is however many bits are in an int on this architecture. multibyte numbers are interpreted in LSB
	unsigned int consume_bits(unsigned char num_bits)
	{
		if (num_bits > (sizeof(int) * 8))
			return 0;

		unsigned int res = 0;
		for (int i = 0; i < num_bits; i++)
		{
			res += ((stream[cur_byte] >> cur_bit) & 0x1) << i;
			inc_bit();
		}
		return res;
	}
	vector<unsigned short> inflate_tree(vector<unsigned short> lens, unsigned char max_len);
	Node *construct_tree(vector<unsigned short> codes, vector<unsigned short> code_lens);
	void destroy_tree(Node *tree);

  public:
	ZStream(vector<unsigned char> &stream);
	Error get_error()
	{
		return err;
	}
	vector<unsigned char> get_data()
	{
		return data;
	}
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
	unsigned char paeth_predictor(unsigned char a, unsigned char b, unsigned char c);

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

#define NPNG_IMPLEMENTATION
#ifdef NPNG_IMPLEMENTATION
ZStream::ZStream(vector<unsigned char> &stream)
{
	this->stream = stream;

	// first six bytes are header

	// compression method and info (first byte)
	unsigned char compression_method = stream[0] & 0x0F;
	unsigned char compression_info = (stream[0] & 0xF0) >> 4;

	// we only support compression method 8 (DEFLATE)
	if (compression_method != 8)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	// calculate the window size
	int window_size = 1;
	for (int i = 0; i < compression_info + 8; i++)
	{
		window_size *= 2;
	}

	// Flags (second byte)
	unsigned char fdict = (stream[1] & 0x20) >> 5;
	unsigned char flevel = (stream[1] & 0xC0) >> 6;

	// we dont support preset dictionaries
	if (fdict == 1)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	// make sure our header is valid
	uint16_t fcheck = (stream[0] << 8) + stream[1];
	if (fcheck % 31 != 0)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	cur_byte += 2;

	while (!done)
	{
		process_block();
		if (err != Error::NONE)
			return;
	}

	// TODO: last 4 bytes are ADLER32 checksum
}

vector<unsigned short> ZStream::inflate_tree(vector<unsigned short> lens, unsigned char max_len)
{
	vector<unsigned short> codes(lens.size(), 0);

	vector<unsigned short> len_freqs(max_len + 1, 0);
	for (int i = 0; i < lens.size(); i++)
	{
		len_freqs[lens[i]]++;
	}

	unsigned short current_code = 0;
	vector<unsigned short> next_code(max_len + 1, 0);
	len_freqs[0] = 0;
	for (char i = 1; i <= max_len; i++)
	{
		current_code = (current_code + len_freqs[i - 1]) << 1;
		next_code[i] = current_code;
	}

	for (int n = 0; n < lens.size(); n++)
	{
		unsigned short len = lens[n];
		if (len != 0)
		{
			codes[n] = next_code[len];
			next_code[len]++;
		}
	}

	return codes;
}

Node *ZStream::construct_tree(vector<unsigned short> codes, vector<unsigned short> code_lens)
{
	Node *root = new Node();

	Node *cur_node = root;
	for (int i = 0; i < codes.size(); i++)
	{
		// Traverse down the tree
		for (int j = 0; j < code_lens[i]; j++)
		{
			int dir = (codes[i] >> (code_lens[i] - j - 1)) & 0x1;

			// 0 is left, 1 is right
			if (dir == 0)
			{
				if (cur_node->left == nullptr)
					cur_node->left = new Node();

				cur_node = cur_node->left;
			}
			else
			{
				if (cur_node->right == nullptr)
					cur_node->right = new Node();

				cur_node = cur_node->right;
			}
		}

		if (code_lens[i] != 0)
		{
			cur_node->data = i;
			cur_node->is_code = true;
		}

		cur_node = root;
	}

	return root;
}

void ZStream::destroy_tree(Node *tree)
{
	if (tree->left)
		destroy_tree(tree->left);
	if (tree->right)
		destroy_tree(tree->right);

	delete tree;
}

vector<unsigned short> ZStream::scan_tree(unsigned short num_codes, Node *code_tree)
{
	vector<unsigned short> scanned_codes;
	while (scanned_codes.size() < num_codes)
	{
		unsigned short scanned_code = scan_code(code_tree);

		if (scanned_code == 16)
		{
			unsigned char repeat = consume_bits(2) + 3;

			for (int i = 0; i < repeat; i++)
				scanned_codes.push_back(scanned_codes.back());
		}
		else if (scanned_code == 17)
		{
			unsigned char repeat = consume_bits(3) + 3;

			for (int i = 0; i < repeat; i++)
				scanned_codes.push_back(0);
		}
		else if (scanned_code == 18)
		{
			unsigned char repeat = consume_bits(7) + 11;

			for (int i = 0; i < repeat; i++)
				scanned_codes.push_back(0);
		}
		else if (scanned_code < 16)
		{
			scanned_codes.push_back(scanned_code);
		}
		else
		{
			err = Error::ZLIB_ERROR;
		}
	}

	return scanned_codes;
}

unsigned short ZStream::scan_code(const Node *tree)
{
	if (tree->is_code)
		return tree->data;

	unsigned char dir = consume_bits(1);
	if (dir == 1)
	{
		return scan_code(tree->right);
	}
	else
	{
		return scan_code(tree->left);
	}
}

// TODO: IMPLEMENT RETURN ERRORS
void ZStream::decompress_block(const Node *len_tree, const Node *dist_tree)
{
	// Loop until end of block code
	while (true)
	{
		unsigned short scanned_code = scan_code(len_tree);

		if (scanned_code >= 0 && scanned_code < 256)
		{
			data.push_back(scanned_code);
		}
		// end of block code
		else if (scanned_code == 256)
		{
			break;
		}
		else if (scanned_code <= 285)
		{
			auto len = len_base_alphabet[scanned_code - 257] + consume_bits(len_extra_bits[scanned_code - 257]);

			unsigned short scanned_dist_code = scan_code(dist_tree);

			auto dist = dist_base_alphabet[scanned_dist_code] + consume_bits(dist_extra_bits[scanned_dist_code]);

			int start = data.size() - dist;
			for (int i = 0; i < len; i++)
			{
				data.push_back(data[start + i]);
			}
		}
		else
		{
			err = Error::ZLIB_ERROR;
			return;
		}
	}
}

void ZStream::process_block()
{
	unsigned char bfinal = consume_bits(1);
	unsigned char btype = consume_bits(2);

	if (bfinal == 1)
		done = true;

	// 3 is reserved
	if (btype == 3)
	{
		err = Error::ZLIB_ERROR;
		return;
	}

	// no compression
	if (btype == 0)
	{
		// skip the remaining bits in the cur byte
		inc_bits(8 - cur_bit);

		uint16_t len = 0;
		len += stream[cur_byte] + (stream[cur_byte + 1] << 8);
		cur_byte += 2;

		uint16_t nlen = 0;
		nlen += stream[cur_byte] + (stream[cur_byte + 1] << 8);
		cur_byte += 2;

		if (len != ~nlen)
		{
			err = Error::ZLIB_ERROR;
			return;
		}

		data.insert(data.end(), &stream[cur_byte], &stream[cur_byte + len]);
	}
	// fixed huffman
	else if (btype == 1)
	{
		vector<unsigned short> lit_code_lengths;
		for (int i = 0; i < 288; i++)
		{
			if (i < 144)
			{
				lit_code_lengths.push_back(8);
			}
			else if (i < 256)
			{
				lit_code_lengths.push_back(9);
			}
			else if (i < 280)
			{
				lit_code_lengths.push_back(7);
			}
			else
			{
				lit_code_lengths.push_back(8);
			}
		}

		vector<unsigned short> dist_code_lengths(32, 5);

		auto lit_codes = inflate_tree(lit_code_lengths, 9);
		auto lit_tree = construct_tree(lit_codes, lit_code_lengths);

		auto dist_codes = inflate_tree(dist_code_lengths, 5);
		auto dist_tree = construct_tree(dist_codes, dist_code_lengths);

		decompress_block(lit_tree, dist_tree);

		destroy_tree(dist_tree);
		destroy_tree(lit_tree);
	}
	// we only consume 2 bits so we should never be over btype 3
	// dynamic huffman (btype 2)
	else
	{
		uint8_t HLIT = consume_bits(5);
		uint8_t HDIST = consume_bits(5);
		uint8_t HCLEN = consume_bits(4);

		vector<unsigned short> code_lens(19, 0);
		for (int i = 0; i < HCLEN + 4; i++)
		{
			unsigned short cur_len = consume_bits(3);
			code_lens[code_lengths_alphabet[i]] = cur_len;
		}

		auto len_codes = inflate_tree(code_lens, 7);
		auto len_codes_tree = construct_tree(len_codes, code_lens);

		auto scanned_lit_and_dist_code_lens = scan_tree(HLIT + HDIST + 257 + 1, len_codes_tree);

		auto scanned_lit_code_lens = vector<unsigned short>{scanned_lit_and_dist_code_lens.begin(),
		                                                    scanned_lit_and_dist_code_lens.begin() + HLIT + 257};
		auto scanned_distance_code_lens = vector<unsigned short>{scanned_lit_and_dist_code_lens.begin() + HLIT + 257,
		                                                         scanned_lit_and_dist_code_lens.end()};

		auto lit_tree_codes = inflate_tree(scanned_lit_code_lens, 15);
		auto distance_tree_codes = inflate_tree(scanned_distance_code_lens, 15);

		auto lit_tree = construct_tree(lit_tree_codes, scanned_lit_code_lens);
		auto distance_tree = construct_tree(distance_tree_codes, scanned_distance_code_lens);

		decompress_block(lit_tree, distance_tree);

		destroy_tree(len_codes_tree);
		destroy_tree(lit_tree);
		destroy_tree(distance_tree);
	}
}

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

	ZStream zs(compressed_data);
	uncompressed_data = zs.get_data();

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
			default:
				err = Error::INVALID_FILTER_TYPE;
				return;
			}
		}
	}
}

unsigned char Image::paeth_predictor(unsigned char a, unsigned char b, unsigned char c)
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
