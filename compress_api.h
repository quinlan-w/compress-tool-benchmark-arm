#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <sys/time.h>
#include "cmdline.h"
#include "zlib.h"
#include "lz4.h"
#include "lz4hc.h"
#include "lz4frame.h"
#include "snappy.h"

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
bool read_file(const char *fn, std::vector<char> &data)
{
    FILE *fp = fopen(fn, "r");
    if (fp != nullptr)
    {
        fseek(fp, 0L, SEEK_END);
        auto len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data.clear();
        size_t read_size = 0;
        if (len > 0)
        {
            data.resize(len);
            read_size = fread(data.data(), 1, len, fp);
        }
        fclose(fp);
        return read_size == (size_t)len;
    }
    return false;
}
bool read_file(FILE *fp, std::vector<char> &data)
{
    if (fp != nullptr)
    {
        fseek(fp, 0L, SEEK_END);
        auto len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data.clear();
        size_t read_size = 0;
        if (len > 0)
        {
            data.resize(len);
            read_size = fread(data.data(), 1, len, fp);
        }
        return read_size == (size_t)len;
    }
    return false;
}

size_t GetContents(FILE *fp, std::string *output)
{

    if (fp == nullptr)
    {
        std::exit(1);
    }
    size_t size = 0;
    output->clear();
    while (!std::feof(fp))
    {
        char buffer[4096];
        size_t bytes_read = std::fread(buffer, 1, sizeof(buffer), fp);
        if (bytes_read == 0 && std::ferror(fp))
        {
            std::perror("fread");
            std::exit(1);
        }
        size += bytes_read;
        output->append(buffer, bytes_read);
    }
    std::fclose(fp);
    return size;
}

inline char *string_as_array(std::string *str)
{
    return str->empty() ? NULL : &*str->begin();
}
/*
**************************** zlib ****************************
*/

#define CHUNK 16384
/* Compress from file source to file dest until EOF on source.
   def() returns Z_OK on success, Z_MEM_ERROR if memory could not be
   allocated for processing, Z_STREAM_ERROR if an invalid compression
   level is supplied, Z_VERSION_ERROR if the version of zlib.h and the
   version of the library linked do not match, or Z_ERRNO if there is
   an error reading or writing the files. */
int zlib_def(FILE *source, FILE *dest, int level)
{
    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    /* allocate deflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, level);
    if (ret != Z_OK)
        return ret;

    /* compress until end of file */
    do
    {
        strm.avail_in = fread(in, 1, CHUNK, source);
        if (ferror(source))
        {
            (void)deflateEnd(&strm);
            return Z_ERRNO;
        }
        flush = feof(source) ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = in;

        /* run deflate() on input until output buffer not full, finish
           compression if all of source has been read in */
        do
        {
            strm.avail_out = CHUNK;
            strm.next_out = out;
            ret = deflate(&strm, flush);   /* no bad return value */
            assert(ret != Z_STREAM_ERROR); /* state not clobbered */
            have = CHUNK - strm.avail_out;
            if (fwrite(out, 1, have, dest) != have || ferror(dest))
            {
                (void)deflateEnd(&strm);
                return Z_ERRNO;
            }
        } while (strm.avail_out == 0);
        assert(strm.avail_in == 0); /* all input will be used */

        /* done when last data in file processed */
    } while (flush != Z_FINISH);
    assert(ret == Z_STREAM_END); /* stream will be complete */

    /* clean up and return */
    (void)deflateEnd(&strm);
    return Z_OK;
}

int zlib_inf(const std::vector<char> &in, std::vector<char> &out, size_t buffer_size)
{
    int ret = 0;
    // std::vector<char> buffer(buffer_size, 0);
    size_t remain_size = out.size();
    z_stream d_stream = {0}; /* decompression stream */
    d_stream.zalloc = NULL;
    d_stream.zfree = NULL;
    d_stream.opaque = NULL;
    d_stream.next_in = (Bytef *)in.data();
    d_stream.next_out = (Bytef *)out.data();

    if (inflateInit(&d_stream) != Z_OK)
        printf("inflateInit error \n");
    while (d_stream.total_out < out.size() &&
           d_stream.total_in < in.size())
    {
        d_stream.avail_in = in.size() - d_stream.total_in;
        d_stream.avail_out = min(remain_size, buffer_size);
        ret = inflate(&d_stream, Z_NO_FLUSH);
        if (ret == Z_STREAM_END)
            break;
        else if (ret != Z_OK)
        {
            printf("inflate failed\n");
            return ret;
        }
        d_stream.next_out = (Bytef *)(out.data() + d_stream.total_out);
        remain_size -= d_stream.total_out;
    }
    ret = inflateEnd(&d_stream);
    if (ret != Z_OK)
    {
        printf("gzip inflateEnd error \n");
    }
    return ret;
}

/*
**************************** lz4 ****************************
*/
int lz4_compress(FILE *in, FILE *out, int level)
{
    std::vector<char> src;
    if (!read_file(in, src))
    {
        printf("lz4 error read\n");
        return -1;
    }
    int src_size = src.size();
    int max_dst_size = LZ4_compressBound(src_size);

    std::vector<char> dst(max_dst_size, 0);
    size_t real_bytes = LZ4_compress_fast(src.data(), dst.data(), src_size, max_dst_size, level);
    if (real_bytes == 0)
    {
        printf("lz4 compress fialed\n");
        return -1;
    }
    fwrite(dst.data(), 1, real_bytes, out);
    return 0;
}

int lz4_decompress(const std::vector<char> &in, std::vector<char> &out, size_t buffer_size)
{
    size_t dst_size = 0;
    dst_size = LZ4_decompress_safe(in.data(), out.data(), in.size(), out.size());
    return dst_size;
}

/*
**************************** lz4 HC ****************************
*/

int lz4hc_compress(FILE *in, FILE *out, int level)
{
    std::vector<char> src;
    if (!read_file(in, src))
    {
        printf("lz4 error read\n");
        return -1;
    }
    int src_size = src.size();
    int max_dst_size = LZ4_compressBound(src_size);

    std::vector<char> dst(max_dst_size, 0);
    size_t real_bytes = LZ4_compress_HC(src.data(), dst.data(), src_size, max_dst_size, level);
    if (real_bytes == 0)
    {
        printf("lz4 compress fialed\n");
        return -1;
    }

    dst.resize(real_bytes);
    char *src_size_str = (char *)&src_size;
    std::vector<char> src_size_v;
    for (int i = 0; i < sizeof(src_size_str); i++)
    {
        src_size_v.push_back(src_size_str[i]);
    }
    std::vector<char> dst_puls;
    dst_puls.insert(dst_puls.end(), src_size_v.begin(), src_size_v.end());
    dst_puls.insert(dst_puls.end(), dst.begin(), dst.end());
    fwrite(dst_puls.data(), 1, dst_puls.size(), out);
    return real_bytes;
}

/**
 *   SNAPPY
 *
 */

int snappy_compress(FILE *in, FILE *out, int level)
{

    std::vector<char> src;
    if (!read_file(in, src))
    {
        printf("snappy error read\n");
        return -1;
    }

    size_t max_compress_length = snappy::MaxCompressedLength(src.size());
    std::vector<char> max_dst(max_compress_length, 0);
    size_t compressed_size = 0;
    snappy::RawCompress(src.data(), src.size(), max_dst.data(), &compressed_size);

    fwrite(max_dst.data(), 1, compressed_size, out);
    return 0;
}

int snappy_decompress(const std::vector<char> &in, std::vector<char> &out, size_t buffer_size)
{
    size_t dst_size = 0;
    if (snappy::RawUncompress(in.data(), in.size(), out.data()) == false)
    {
        return dst_size;
    }
    if (snappy::GetUncompressedLength(in.data(), in.size(), &dst_size) == false)
    {
        return dst_size;
    }
    return dst_size;
}

enum COMPRESS_TYPE
{
    ZLIB,
    LZ4,
    LZ4HC,
    SNAPPY
};

struct COMPRESS_META
{
    COMPRESS_TYPE type;
    /* zlib level */
    int level;
    /* stream buffer size */
    int buffer_size;
};

int compress(FILE *src, FILE *dst, COMPRESS_META &meta)
{
    int ret = 0;
    switch (meta.type)
    {
    case ZLIB:
        if (zlib_def(src, dst, meta.level) != Z_OK)
            ret = -1;
        break;
    case LZ4:
        if (lz4_compress(src, dst, meta.level) == 0)
            ret = -1;
        break;
    case LZ4HC:
        if (lz4hc_compress(src, dst, meta.level) == 0)
            ret = -1;
        break;
    case SNAPPY:
        if (snappy_compress(src, dst, meta.level) == 0)
            ret = -1;
        break;
    default:
        break;
    }
    return 0;
}

int decompress(std::vector<char> &in, std::vector<char> &out, COMPRESS_META &meta)
{
    int ret = 0;
    switch (meta.type)
    {
    case ZLIB:
        if (zlib_inf(in, out, meta.buffer_size) < 0)
            ret = -1;
        break;
    case LZ4:
    case LZ4HC:
        if (lz4_decompress(in, out, meta.buffer_size) == 0)
            ret = -1;
        break;
    case SNAPPY:
        if (snappy_decompress(in, out, meta.buffer_size) == 0)
            ret = -1;
        break;
    default:
        break;
    }
    return ret;
}