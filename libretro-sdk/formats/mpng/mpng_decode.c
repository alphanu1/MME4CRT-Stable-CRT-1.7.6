#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <boolean.h>

#ifdef WANT_MINIZ
#define MINIZ_HEADER_FILE_ONLY
#include "miniz.c"
#endif

#include <formats/mpng.h>

static uint32_t dword_be(const uint8_t *buf)
{
   return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3] << 0);
}

static uint16_t word_be(const uint8_t *buf)
{
   return (buf[0] << 16) | (buf[1] << 8) | (buf[2] << 0);
}

enum mpng_chunk_type
{
   MPNG_CHUNK_TRNS = 0x74524E53,
   MPNG_CHUNK_IHDR = 0x49484452,
   MPNG_CHUNK_IDAT = 0x49444154,
   MPNG_CHUNK_PLTE = 0x504c5445,
   MPNG_CHUNK_IEND = 0x49454e44,
};

struct mpng_ihdr
{
   uint32_t width;
   uint32_t height;
   uint8_t depth;
   uint8_t color_type;
   uint8_t compression;
   uint8_t filter;
   uint8_t interlace;
};

static const uint8_t mpng_magic[8] = {
   0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
};

struct mpng_chunk
{
   uint32_t size;
   uint32_t type;
   const uint8_t *data;
};

bool mpng_parse_ihdr(struct mpng_ihdr *ihdr, struct mpng_chunk *chunk,
      enum video_format format, unsigned int *bpl,
      uint8_t *pixels, uint8_t *pixelsat, uint8_t *pixelsend)
{
   ihdr->width       = dword_be(chunk->data + 0);
   ihdr->height      = dword_be(chunk->data + 4);
   ihdr->depth       = chunk->data[8];
   ihdr->color_type  = chunk->data[9];
   ihdr->compression = chunk->data[10];
   ihdr->filter      = chunk->data[11];
   ihdr->interlace   = chunk->data[12];

   if (ihdr->width == 0 || ihdr->height == 0)
      return false;

   if (ihdr->width >= 0x80000000)
      return false;

   if (ihdr->height >= 0x80000000)
      return false;

   if (     ihdr->color_type != 2
         && ihdr->color_type != 3
         && ihdr->color_type != 6)
      return false;


   if (ihdr->compression != 0)
      return false;
   if (ihdr->filter != 0)
      return false;
   if (ihdr->interlace != 0 && ihdr->interlace != 1)
      return false;

   /*
    * Greyscale 	0
    * Truecolour 	2
    * Indexed-colour 	3
    * Greyscale with alpha 	4
    * Truecolour with alpha 	6
    **/

   switch (ihdr->color_type)
   {
      case 2:
         /* Truecolor; can be 16bpp but I don't want that. */
         if (ihdr->depth != 8)
            return false;
         *bpl = 3 * ihdr->width;
         break;
      case 3:
         /* Paletted. */
         if (ihdr->depth != 1
               && ihdr->depth != 2
               && ihdr->depth != 4
               && ihdr->depth != 8)
            return false;
         *bpl = (ihdr->width * ihdr->depth + ihdr->depth - 1) / 8;
         break;
      case 6:
         /* Truecolor with alpha. */
         if (ihdr->depth != 8)
            return false;

         /* Can only decode alpha on ARGB formats. */
         if (format != FMT_ARGB8888)
            return false;
         *bpl = 4 * ihdr->width;
         break;
   }

   pixels    = (uint8_t*)malloc((*bpl + 1) * ihdr->height);

   if (!pixels)
      return false;

   pixelsat  = (uint8_t*)pixels;
   pixelsend = (uint8_t*)(pixels + (*bpl + 1) * ihdr->height);

   return true;
}

static bool mpng_read_plte(struct mpng_ihdr *ihdr,
      struct mpng_chunk *chunk,
      uint8_t *pixels,
      uint32_t *buffer, unsigned entries)
{
   unsigned i;
   if (chunk->size % 3)
      return false;
   if (!pixels || entries != 0)
      return false;
   if (chunk->size == 0 || chunk->size > 3 * 256)
      return false;

   /* Palette on RGB is allowed but rare, 
    * and it's just a recommendation anyways. */
   if (ihdr->color_type != 3)
      return true; /* not sure about this - Alcaro review? */

   for (i = 0; i < entries; i++)
   {
      uint32_t r = chunk->data[3 * i + 0];
      uint32_t g = chunk->data[3 * i + 1];
      uint32_t b = chunk->data[3 * i + 2];
      buffer[i] = (r << 16) | (g << 8) | (b << 0) | (0xffu << 24);
   }

   return true;
}

bool png_decode_iterate(const uint8_t *data, const uint8_t *data_end,
      struct mpng_ihdr *ihdr, struct mpng_image *img,
      uint32_t *palette, enum video_format format,
      unsigned int *bpl, int *palette_len, uint8_t *pixels,
      uint8_t *pixelsat, uint8_t *pixelsend
#ifdef WANT_MINIZ
      ,tinfl_decompressor *inflator
#endif
      )
{
   unsigned int chunkchecksum;
   unsigned int actualchunkchecksum;
   struct mpng_chunk chunk = {0};

   if ((data + 4 + 4) > data_end)
      return -1;

   chunk.size  = dword_be(data);
   chunk.type  = dword_be(data + 4);

   if (chunk.size >= 0x80000000)
      return -1;
   if ((data + 4 + chunk.size + 4) > data_end)
      return -1;

   chunkchecksum       = mz_crc32(mz_crc32(0, NULL, 0), (uint8_t*)data+4, 4 + chunk.size);
   chunk.data          = (const uint8_t*)(data + 4 + 4);
   actualchunkchecksum = dword_be(data + 4 + 4 + chunk.size);

   if (actualchunkchecksum != chunkchecksum)
      return -1;

   data += 4 + 4 + chunk.size + 4;

   switch (chunk.type)
   {
      case MPNG_CHUNK_IHDR:
         if (!mpng_parse_ihdr(ihdr, &chunk, format, bpl, pixels,
                  pixelsat, pixelsend))
            return -1;
         break;
      case MPNG_CHUNK_PLTE:
         *palette_len = chunk.size / 3;
         if (!mpng_read_plte(ihdr, &chunk, pixels, palette, *palette_len))
            return -1;
         break;
      case MPNG_CHUNK_TRNS:
         if (format != FMT_ARGB8888 || !pixels || pixels != pixelsat)
            return -1;

         if (ihdr->color_type == 2)
         {
            if (*palette_len == 0)
               return -1;
            return -1;
         }
         else if (ihdr->color_type == 3)
            return -1;
         else
            return -1;
         break;
      case MPNG_CHUNK_IDAT:
         {
            size_t byteshere;
            size_t chunklen_copy;
#ifdef WANT_MINIZ
            tinfl_status status;
#endif

            if (!pixels || (ihdr->color_type == 3 && (*palette_len) == 0))
               return -1;

            chunklen_copy       = chunk.size;
            byteshere           = (pixelsend - pixelsat)+1;

#ifdef WANT_MINIZ
            status = tinfl_decompress(inflator,
                  (const mz_uint8 *)chunk.data,
                  &chunklen_copy, pixels,
                  pixelsat,
                  &byteshere,
                  TINFL_FLAG_HAS_MORE_INPUT | 
                  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | 
                  TINFL_FLAG_PARSE_ZLIB_HEADER);
#endif

            pixelsat += byteshere;

#ifdef WANT_MINIZ
            if (status < TINFL_STATUS_DONE)
               return -1;
#endif
         }
         break;
      case MPNG_CHUNK_IEND:
         {
            unsigned b, x, y;
#ifdef WANT_MINIZ
            tinfl_status status;
#endif
            size_t finalbytes;
            unsigned int bpp_packed;
            uint8_t *prevout;
            size_t            zero = 0;
            uint8_t *          out = NULL;
            uint8_t * filteredline = NULL;

            if (data != data_end)
               return -1;
            if (chunk.size)
               return -1;

            finalbytes = (pixelsend - pixelsat);

#ifdef WANT_MINIZ
            status = tinfl_decompress(inflator, (const mz_uint8 *)NULL, &zero, pixels, pixelsat, &finalbytes,
                  TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_PARSE_ZLIB_HEADER);
#endif

            pixelsat += finalbytes;

#ifdef WANT_MINIZ
            if (status < TINFL_STATUS_DONE)
               return -1;

            if (status > TINFL_STATUS_DONE)
               return -1;
#endif
            /* Too little data (can't be too much 
             * because we didn't give it that buffer size) */
            if (pixelsat != pixelsend)
               return -1; 

            out = (uint8_t*)malloc(videofmt_byte_per_pixel(format) * ihdr->width * ihdr->height);

            if (!out)
               return -1;

            /* TODO: deinterlace at random point */

            /* run filters */
            bpp_packed = ((ihdr->color_type == 2) ? 3 : (ihdr->color_type == 6) ? 4 : 1);
            prevout    = (out + (4 * ihdr->width * 1));

            /* This will blow up if a 1px high image 
             * is filtered with Paeth, but highly unlikely. */
            if (ihdr->height==1)
               prevout = out;

            /* Not using bpp here because we only need a chunk of black anyways */
            memset(prevout, 0, 4 * ihdr->width * 1);

            filteredline = pixels;

            for (y = 0; y < ihdr->height; y++)
            {
               uint8_t *thisout = (uint8_t*)(out + ((*bpl) * y));

               switch (*(filteredline++))
               {
                  case 0:
                     memcpy(thisout, filteredline, (*bpl));
                     break;
                  case 1:
                     memcpy(thisout, filteredline, bpp_packed);

                     for (x = bpp_packed; x < (*bpl); x++)
                        thisout[x] = thisout[x - bpp_packed] + filteredline[x];
                     break;
                  case 2:
                     for (x = 0; x < (*bpl); x++)
                        thisout[x] = prevout[x] + filteredline[x];
                     break;
                  case 3:
                     for (x = 0; x < bpp_packed; x++)
                     {
                        int a      = 0;
                        int b      = prevout[x];
                        thisout[x] = (a+b)/2 + filteredline[x];
                     }
                     for (x = bpp_packed; x < (*bpl); x++)
                     {
                        int a      = thisout[x - bpp_packed];
                        int b      = prevout[x];
                        thisout[x] = (a + b) / 2 + filteredline[x];
                     }
                     break;
                  case 4:
                     for (x = 0; x < bpp_packed; x++)
                     {
                        int prediction;

                        int a   = 0;
                        int b   = prevout[x];
                        int c   = 0;

                        int p   = a+b-c;
                        int pa  = abs(p-a);
                        int pb  = abs(p-b);
                        int pc  = abs(p-c);

                        if (pa <= pb && pa <= pc)
                           prediction=a;
                        else if (pb <= pc)
                           prediction=b;
                        else
                           prediction=c;

                        thisout[x] = filteredline[x]+prediction;
                     }

                     for (x = bpp_packed; x < (*bpl); x++)
                     {
                        int prediction;

                        int a   = thisout[x - bpp_packed];
                        int b   = prevout[x];
                        int c   = prevout[x - bpp_packed];

                        int p   = a+b-c;
                        int pa  = abs(p-a);
                        int pb  = abs(p-b);
                        int pc  = abs(p-c);

                        if (pa <= pb && pa <= pc)
                           prediction = a;
                        else if (pb <= pc)
                           prediction = b;
                        else
                           prediction = c;

                        thisout[x] = filteredline[x] + prediction;
                     }
                     break;
                  default:
                     return -1;
               }
               prevout       = thisout;
               filteredline += (*bpl);
            }

            /* Unpack paletted data
             * not sure if these aliasing tricks are valid,
             * but the prerequisites for that bugging up 
             * are pretty much impossible to hit.
             **/
            if (ihdr->color_type == 3)
            {
               switch (ihdr->depth)
               {
                  case 1:
                     {
                        int y          = ihdr->height;
                        uint8_t * outp = out + 3 * ihdr->width * ihdr->height;
                        do
                        {
                           uint8_t * inp = (uint8_t*)(out + y * (*bpl));
                           int x         = (ihdr->width + 7) / 8;
                           do
                           {
                              x--;
                              inp--;
                              for (b = 0; b < 8; b++)
                              {
                                 int rgb32 = palette[((*inp)>>b)&1];
                                 *(--outp) = rgb32 >> 0;
                                 *(--outp) = rgb32 >> 8;
                                 *(--outp) = rgb32 >> 16;
                              }
                           } while(x);
                           y--;
                        } while(y);
                     }
                     break;
                  case 2:
                     {
                        int y          = ihdr->height;
                        uint8_t * outp = (uint8_t*)(out + 3 * ihdr->width * ihdr->height);
                        do
                        {
                           unsigned char *inp = out + y * (*bpl);
                           int x              = (ihdr->width + 3) / 4;
                           do
                           {
                              int b;
                              x--;
                              inp--;
                              for (b = 0;b < 8; b += 2)
                              {
                                 int rgb32 = palette[((*inp)>>b)&3];
                                 *(--outp) = rgb32 >> 0;
                                 *(--outp) = rgb32 >> 8;
                                 *(--outp) = rgb32 >> 16;
                              }
                           } while(x);
                           y--;
                        } while(y);
                     }
                     break;
                  case 4:
                     {
                        int y         = ihdr->height;
                        uint8_t *outp = out + 3 * ihdr->width * ihdr->height;

                        do
                        {
                           unsigned char *inp = out + y * (*bpl);
                           int x              = (ihdr->width + 1) / 2;

                           do
                           {
                              int rgb32;

                              x--;
                              inp--;
                              rgb32     = palette[*inp&15];
                              *(--outp) = rgb32 >> 0;
                              *(--outp) = rgb32 >> 8;
                              *(--outp) = rgb32 >> 16;
                              rgb32     = palette[*inp>>4];
                              *(--outp) = rgb32 >> 0;
                              *(--outp) = rgb32 >> 8;
                              *(--outp) = rgb32 >> 16;
                           } while(x);
                           y--;
                        } while(y);
                     }
                     break;
                  case 8:
                     {
                        uint8_t *inp  = (uint8_t*)(out + ihdr->width * ihdr->height);
                        uint8_t *outp = (uint8_t*)(out + 3 * ihdr->width * ihdr->height);
                        int i         = ihdr->width * ihdr->height;
                        do
                        {
                           int rgb32;
                           i--;
                           inp       -= 1;
                           rgb32      = palette[*inp];

                           *(--outp)  = rgb32 >> 0;
                           *(--outp)  = rgb32 >> 8;
                           *(--outp)  = rgb32 >> 16;
                        } while(i);
                     }
                     break;
               }
            }

            /* unpack to 32bpp if requested */
            if (format != FMT_RGB888 && ihdr->color_type == 2)
            {
               uint8_t  *inp   = (uint8_t*)(out + ihdr->width * ihdr->height * 3);
               uint32_t *outp  = (uint32_t*)(((uint32_t*)out) + ihdr->width * ihdr->height);
               int i           = ihdr->width * ihdr->height;

               do
               {
                  i--;
                  inp-=3;
                  outp--;
                  *outp = word_be(inp) | 0xFF000000;
               } while(i);
            }

            img->width    = ihdr->width;
            img->height   = ihdr->height;
            img->pixels   = out;
            img->pitch    = videofmt_byte_per_pixel(format) * ihdr->width;
            img->format   = format;
            free(pixels);
            return 1;
         }
         break;
      default:
         if (!(chunk.type & 0x20000000))
            return -1; /* unknown critical */
         /* otherwise ignore */
   }

   return 0;
}

bool png_decode(const void *userdata, size_t len,
      struct mpng_image *img, enum video_format format)
{
   struct mpng_ihdr ihdr = {0};
   unsigned int bpl;
   uint32_t palette[256];
   int palette_len = 0;
   uint8_t *pixelsat = NULL;
   uint8_t *pixelsend = NULL;
   uint8_t *pixels = NULL;
   const uint8_t *data_end = NULL;
   const uint8_t *data = (const uint8_t*)userdata;

   if (!data)
      return false;

   memset(img, 0, sizeof(struct mpng_image));

   /* Only works for RGB888, XRGB8888, and ARGB8888 */
   if      (format != FMT_RGB888 
         && format != FMT_XRGB8888
         && format != FMT_ARGB8888)
      return false;

   if (len < 8)
      return false;

   if (memcmp(data, mpng_magic, sizeof(mpng_magic)) != 0)
      return false;

   data_end  = (const uint8_t*)(data + len);
   data     += 8;

   memset(palette, 0, sizeof(palette));

#ifdef WANT_MINIZ
   tinfl_decompressor inflator;
   tinfl_init(&inflator);
#endif

   while (1)
   {
      int ret = png_decode_iterate(data, data_end,
            &ihdr, img, palette, format, &bpl, &palette_len,
            pixels, pixelsat, pixelsend
#ifdef WANT_MINIZ
            ,&inflator
#endif
            );

      switch (ret)
      {
         case -1:
            goto error;
         case 1:
            return true;
         default:
            break;
      }
   }

error:
   free(pixels);
   memset(img, 0, sizeof(struct mpng_image));
   return false;
}
