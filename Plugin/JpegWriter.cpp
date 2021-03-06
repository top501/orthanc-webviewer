/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2015 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "JpegWriter.h"

#include "../Orthanc/OrthancException.h"

#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <vector>
#include <string.h>
#include <stdlib.h>

namespace OrthancPlugins
{
  namespace
  {
    class ErrorManager 
    {
    private:
      struct jpeg_error_mgr pub;  /* "public" fields */
      jmp_buf setjmp_buffer;      /* for return to caller */
      std::string message;

      static void OutputMessage(j_common_ptr cinfo)
      {
        char message[JMSG_LENGTH_MAX];
        (*cinfo->err->format_message) (cinfo, message);

        ErrorManager* that = reinterpret_cast<ErrorManager*>(cinfo->err);
        that->message = std::string(message);
      }


      static void ErrorExit(j_common_ptr cinfo)
      {
        (*cinfo->err->output_message) (cinfo);

        ErrorManager* that = reinterpret_cast<ErrorManager*>(cinfo->err);
        longjmp(that->setjmp_buffer, 1);
      }
      

    public:
      ErrorManager()
      {
        memset(&pub, 0, sizeof(struct jpeg_error_mgr));
        memset(&setjmp_buffer, 0, sizeof(jmp_buf));

        jpeg_std_error(&pub);
        pub.error_exit = ErrorExit;
        pub.output_message = OutputMessage;
      }

      struct jpeg_error_mgr* GetPublic()
      {
        return &pub;
      }

      jmp_buf& GetJumpBuffer()
      {
        return setjmp_buffer;
      }

      const std::string& GetMessage() const
      {
        return message;
      }
    };
  }


  static void GetLines(std::vector<uint8_t*>& lines,
                       unsigned int height,
                       unsigned int pitch,
                       Orthanc::PixelFormat format,
                       const void* buffer)
  {
    if (format != Orthanc::PixelFormat_Grayscale8 &&
        format != Orthanc::PixelFormat_RGB24)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
    }

    lines.resize(height);

    uint8_t* base = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(buffer));
    for (unsigned int y = 0; y < height; y++)
    {
      lines[y] = base + static_cast<intptr_t>(y) * static_cast<intptr_t>(pitch);
    }
  }


  static void Compress(struct jpeg_compress_struct& cinfo,
                       std::vector<uint8_t*>& lines,
                       unsigned int width,
                       unsigned int height,
                       Orthanc::PixelFormat format,
                       int quality)
  {
    cinfo.image_width = width;
    cinfo.image_height = height;

    switch (format)
    {
      case Orthanc::PixelFormat_Grayscale8:
        cinfo.input_components = 1;
        cinfo.in_color_space = JCS_GRAYSCALE;
        break;

      case Orthanc::PixelFormat_RGB24:
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        break;

      default:
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    jpeg_write_scanlines(&cinfo, &lines[0], height);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
  }
                       

  void JpegWriter::SetQuality(uint8_t quality)
  {
    if (quality <= 0 || quality > 100)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
    }

    quality_ = quality;
  }


  void JpegWriter::WriteToFile(const char* filename,
                               unsigned int width,
                               unsigned int height,
                               unsigned int pitch,
                               Orthanc::PixelFormat format,
                               const void* buffer)
  {
    FILE* fp = fopen(filename, "wb");
    if (fp == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_FullStorage);
    }

    std::vector<uint8_t*> lines;
    GetLines(lines, height, pitch, format, buffer);

    struct jpeg_compress_struct cinfo;
    memset(&cinfo, 0, sizeof(struct jpeg_compress_struct));

    ErrorManager jerr;
    cinfo.err = jerr.GetPublic();

    if (setjmp(jerr.GetJumpBuffer())) 
    {
      /* If we get here, the JPEG code has signaled an error.
       * We need to clean up the JPEG object, close the input file, and return.
       */
      jpeg_destroy_compress(&cinfo);
      fclose(fp);
      throw Orthanc::OrthancException("Error during JPEG encoding: " + jerr.GetMessage());
    }

    // Do not allocate data on the stack below this line!

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    Compress(cinfo, lines, width, height, format, quality_);

    // Everything went fine, "setjmp()" didn't get called

    fclose(fp);
  }


  void JpegWriter::WriteToMemory(std::string& jpeg,
                                 unsigned int width,
                                 unsigned int height,
                                 unsigned int pitch,
                                 Orthanc::PixelFormat format,
                                 const void* buffer)
  {
    std::vector<uint8_t*> lines;
    GetLines(lines, height, pitch, format, buffer);

    struct jpeg_compress_struct cinfo;
    memset(&cinfo, 0, sizeof(struct jpeg_compress_struct));

    ErrorManager jerr;

    unsigned char* data = NULL;
    unsigned long size;

    if (setjmp(jerr.GetJumpBuffer())) 
    {
      jpeg_destroy_compress(&cinfo);

      if (data != NULL)
      {
        free(data);
      }

      throw Orthanc::OrthancException("Error during JPEG encoding: " + jerr.GetMessage());
    }

    // Do not allocate data on the stack below this line!

    jpeg_create_compress(&cinfo);
    cinfo.err = jerr.GetPublic();
    jpeg_mem_dest(&cinfo, &data, &size);

    Compress(cinfo, lines, width, height, format, quality_);

    // Everything went fine, "setjmp()" didn't get called

    jpeg.assign(reinterpret_cast<const char*>(data), size);
    free(data);
  }
}
