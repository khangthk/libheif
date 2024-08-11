/*
 * HEIF JPEG codec.
 * Copyright (c) 2023 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "jpeg.h"
#include <string>
#include "security_limits.h"
#include <pixelimage.h>
#include <libheif/api_structs.h>
#include <color-conversion/colorconversion.h>
#include <cstring>


std::string Box_jpgC::dump(Indent& indent) const
{
  std::ostringstream sstr;
  sstr << Box::dump(indent);

  sstr << indent << "num bytes: " << m_data.size() << "\n";

  return sstr.str();
}


Error Box_jpgC::write(StreamWriter& writer) const
{
  size_t box_start = reserve_box_header_space(writer);

  writer.write(m_data);

  prepend_header(writer, box_start);

  return Error::Ok;
}


Error Box_jpgC::parse(BitstreamRange& range)
{
  if (!has_fixed_box_size()) {
    return Error{heif_error_Unsupported_feature, heif_suberror_Unspecified, "jpgC with unspecified size are not supported"};
  }

  size_t nBytes = range.get_remaining_bytes();
  if (nBytes > MAX_MEMORY_BLOCK_SIZE) {
    return Error{heif_error_Invalid_input, heif_suberror_Unspecified, "jpgC block exceeds maximum size"};
  }

  m_data.resize(nBytes);
  range.read(m_data.data(), nBytes);
  return range.get_error();
}


Result<ImageItem::CodedImageData> ImageItem_JPEG::encode(const std::shared_ptr<HeifPixelImage>& image,
                                                         struct heif_encoder* encoder,
                                                         const struct heif_encoding_options& options,
                                                         enum heif_image_input_class input_class)
{
  return encode_image_as_jpeg(image, encoder, options, input_class);
}


Result<ImageItem::CodedImageData> ImageItem_JPEG::encode_image_as_jpeg(const std::shared_ptr<HeifPixelImage>& image,
                                                                       struct heif_encoder* encoder,
                                                                       const struct heif_encoding_options& options,
                                                                       enum heif_image_input_class input_class)
{
  CodedImageData codedImage;

  // --- check whether we have to convert the image color space

  // JPEG always uses CCIR-601

  heif_color_profile_nclx target_heif_nclx;
  target_heif_nclx.matrix_coefficients = heif_matrix_coefficients_ITU_R_BT_601_6;
  target_heif_nclx.color_primaries = heif_color_primaries_ITU_R_BT_601_6;
  target_heif_nclx.transfer_characteristics = heif_transfer_characteristic_ITU_R_BT_601_6;
  target_heif_nclx.full_range_flag = true;

  printf("e_j 1\n");

  Result<std::shared_ptr<HeifPixelImage>> srcImageResult = convert_colorspace_for_encoding(image, encoder, options, &target_heif_nclx);
  if (srcImageResult.error) {
    printf("err1 %s\n", srcImageResult.error.message.c_str());
    return srcImageResult.error;
  }

  std::shared_ptr<HeifPixelImage> src_image = srcImageResult.value;
  printf("p: %p\n", src_image.get());
  codedImage.encoded_image = src_image;

  // --- choose which color profile to put into 'colr' box

  add_color_profile(image, options, input_class, &target_heif_nclx, codedImage);


  heif_image c_api_image;
  c_api_image.image = src_image;

  struct heif_error err = encoder->plugin->encode_image(encoder->encoder, &c_api_image, input_class);
  if (err.code) {
    return Error(err.code,
                 err.subcode,
                 err.message);
  }

  std::vector<uint8_t> vec;

  for (;;) {
    uint8_t* data;
    int size;

    encoder->plugin->get_compressed_data(encoder->encoder, &data, &size, nullptr);

    if (data == nullptr) {
      break;
    }

    size_t oldsize = vec.size();
    vec.resize(oldsize + size);
    memcpy(vec.data() + oldsize, data, size);
  }

#if 0
  // Optional: split the JPEG data into a jpgC box and the actual image data.
  // Currently disabled because not supported yet in other decoders.
  if (false) {
    size_t pos = find_jpeg_marker_start(vec, JPEG_SOS);
    if (pos > 0) {
      std::vector<uint8_t> jpgC_data(vec.begin(), vec.begin() + pos);
      auto jpgC = std::make_shared<Box_jpgC>();
      jpgC->set_data(jpgC_data);

      auto ipma_box = m_heif_file->get_ipma_box();
      int index = m_heif_file->get_ipco_box()->find_or_append_child_box(jpgC);
      ipma_box->add_property_for_item_ID(image_id, Box_ipma::PropertyAssociation{true, uint16_t(index + 1)});

      std::vector<uint8_t> image_data(vec.begin() + pos, vec.end());
      vec = std::move(image_data);
    }
  }
#endif

  codedImage.bitstream = vec;

#if 0
  // TODO: extract 'jpgC' header data
#endif

  uint32_t input_width, input_height;
  input_width = src_image->get_width();
  input_height = src_image->get_height();

  // Note: 'ispe' must be before the transformation properties

  auto ispe = std::make_shared<Box_ispe>();
  ispe->set_size(input_width, input_height);
  codedImage.properties.push_back(ispe);

  uint32_t encoded_width = input_width, encoded_height = input_height;

  if (encoder->plugin->plugin_api_version >= 3 &&
      encoder->plugin->query_encoded_size != nullptr) {

    encoder->plugin->query_encoded_size(encoder->encoder,
                                        input_width, input_height,
                                        &encoded_width,
                                        &encoded_height);
  }

  if (input_width != encoded_width ||
      input_height != encoded_height) {

    auto clap = std::make_shared<Box_clap>();
    clap->set(input_width, input_height, encoded_width, encoded_height);
    codedImage.properties.push_back(clap);
  }

  return {codedImage};
}
