#include <k4a/k4a.h>
#include <k4arecord/playback.hpp>
#include <k4arecord/record.hpp>
#include <k4ainternal/k4aplugin.h>
#include <k4ainternal/deloader.h>
#include <k4ainternal/logging.h>

#include "cmdparser.h"
#include "assert.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <iostream>
#include <fstream>
#include <chrono>

extern "C" {
// dynlib includes the logger
char K4A_ENV_VAR_LOG_TO_A_FILE[] = K4A_ENABLE_LOG_TO_A_FILE;
}

static k4a_depth_engine_mode_t get_de_mode_from_depth_mode(k4a_depth_mode_t mode)
{
    k4a_depth_engine_mode_t de_mode;

    switch (mode)
    {
    case K4A_DEPTH_MODE_NFOV_2X2BINNED:
        de_mode = K4A_DEPTH_ENGINE_MODE_LT_SW_BINNING;
        break;
    case K4A_DEPTH_MODE_WFOV_2X2BINNED:
        de_mode = K4A_DEPTH_ENGINE_MODE_QUARTER_MEGA_PIXEL;
        break;
    case K4A_DEPTH_MODE_NFOV_UNBINNED:
        de_mode = K4A_DEPTH_ENGINE_MODE_LT_NATIVE;
        break;
    case K4A_DEPTH_MODE_WFOV_UNBINNED:
        de_mode = K4A_DEPTH_ENGINE_MODE_MEGA_PIXEL;
        break;
    case K4A_DEPTH_MODE_PASSIVE_IR:
        de_mode = K4A_DEPTH_ENGINE_MODE_PCM;
        break;
    default:
        assert(0);
        de_mode = K4A_DEPTH_ENGINE_MODE_UNKNOWN;
    }
    return de_mode;
}

static k4a_depth_engine_input_type_t get_input_format_from_depth_mode(k4a_depth_mode_t mode)
{
    k4a_depth_engine_mode_t de_mode = get_de_mode_from_depth_mode(mode);
    k4a_depth_engine_input_type_t format;

    format = K4A_DEPTH_ENGINE_INPUT_TYPE_12BIT_COMPRESSED;
    if (de_mode == K4A_DEPTH_ENGINE_MODE_MEGA_PIXEL)
    {
        format = K4A_DEPTH_ENGINE_INPUT_TYPE_8BIT_COMPRESSED;
    }
    return format;
}

int main(int argc, char **argv)
{
    k4a::playback input;
    char *output_filename = "out_depth.mkv";

    CmdParser::OptionParser cmd_parser;
    cmd_parser.RegisterOption("-h|--help", "Prints this help", [&]() {
        cmd_parser.PrintOptions();
        exit(0);
    });
    cmd_parser.RegisterOption("-i|--infile", "Specify the input file", 1, [&](const std::vector<char *> &args) {
        input = k4a::playback::open(args[0]);
    });
    cmd_parser.RegisterOption("-o|--outfile",
                              "Specify the output file (default out_depth.mkv)",
                              1,
                              [&](const std::vector<char *> &args) { output_filename = args[0]; });

    int args_left = 0;
    try
    {
        args_left = cmd_parser.ParseCmd(argc, argv);
    }
    catch (CmdParser::ArgumentError &e)
    {
        std::cerr << e.option() << ": " << e.what() << std::endl;
        return 1;
    }

    if (args_left != 0)
    {
        std::cout << "Invalid Options" << std::endl;
        cmd_parser.PrintOptions();
        return 1;
    }

    if (!input.is_valid())
    {
        std::cout << "Invalid Input File" << std::endl;
        cmd_parser.PrintOptions();
        return 1;
    }

    k4a::capture input_capture;
    k4a::image raw_img;
    k4a_record_configuration_t input_config = input.get_record_configuration();
    std::vector<uint8_t> ccb;

    if (!input.get_attachment("depth_cal.ccb", &ccb))
    {
        std::cout << "No Depth Calibration Found" << std::endl;
        return 1;
    }

    std::string ir_tag;
    if (!input.get_tag("K4A_IR_MODE", &ir_tag))
    {
        std::cout << "K4A_IR_MODE Tag Not Found" << std::endl;
        return 1;
    }

    if (ir_tag.compare("RAW") != 0)
    {
        std::cout << "K4A_IR_MODE Tag <" << ir_tag << "> does not match <RAW>" << std::endl;
        return 1;
    }

    k4a::record recorder;
    k4a_depth_engine_context_t *de_context_ptr = nullptr;

    k4a::device nulldev(nullptr); // create a null device for recorder
    k4a_device_configuration_t dev_config;
    dev_config.color_format = input_config.color_format;
    dev_config.color_resolution = input_config.color_resolution;
    dev_config.depth_mode = input_config.depth_mode;
    dev_config.camera_fps = input_config.camera_fps;
    dev_config.synchronized_images_only = false;
    dev_config.depth_delay_off_color_usec = input_config.depth_delay_off_color_usec;
    dev_config.wired_sync_mode = input_config.wired_sync_mode;
    dev_config.subordinate_delay_off_master_usec = input_config.subordinate_delay_off_master_usec;
    dev_config.disable_streaming_indicator = false;
    dev_config.record_raw_depth = false;

    recorder = k4a::record::create(output_filename, nulldev, dev_config);

    // write the calibration to the device
    std::vector<uint8_t> transform_cal = input.get_raw_calibration();
    recorder.add_attachment("calibration.json", transform_cal.data(), transform_cal.size());

    if (input_config.imu_track_enabled)
    {
        recorder.add_imu_track();
    }
    recorder.write_header();

    k4a_depth_engine_result_code_t result =
        deloader_depth_engine_create_and_initialize(&de_context_ptr,
                                                    ccb.size(),
                                                    ccb.data(),
                                                    get_de_mode_from_depth_mode(input_config.depth_mode),
                                                    get_input_format_from_depth_mode(input_config.depth_mode),
                                                    nullptr,
                                                    nullptr,
                                                    nullptr);

    if (result != K4A_DEPTH_ENGINE_RESULT_SUCCEEDED)
    {
        std::cout << "Depth Engine Failure" << std::endl;
        return 1;
    }

    // write the imu data
    k4a_imu_sample_t imu;
    while (input.get_next_imu_sample(&imu))
    {
        recorder.write_imu_sample(imu);
    }

    // write the depth data
    int nframes = 0;
    uint64_t process_time_ms = 0;
    std::cout << "Depth Mode: " << input_config.depth_mode << std::endl;
    std::cout << "Processing Frame ";
    while (input.get_next_capture(&input_capture))
    {
        std::cout << nframes << "..";
        raw_img = input_capture.get_ir_image();

        size_t output_size = deloader_depth_engine_get_output_frame_size(de_context_ptr);
        std::vector<uint8_t> output_buf(output_size);
        k4a_depth_engine_output_frame_info_t outputCaptureInfo = { 0 };

        auto t0 = std::chrono::high_resolution_clock::now();
        result =
            deloader_depth_engine_process_frame(de_context_ptr,
                                                raw_img.get_buffer(),
                                                raw_img.get_size(),
                                                k4a_depth_engine_output_type_t::K4A_DEPTH_ENGINE_OUTPUT_TYPE_Z_DEPTH,
                                                output_buf.data(),
                                                output_buf.size(),
                                                &outputCaptureInfo,
                                                nullptr);

        auto t1 = std::chrono::high_resolution_clock::now();
        process_time_ms += std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (result == K4A_DEPTH_ENGINE_RESULT_SUCCEEDED)
        {
            // create new capture with the color/imu/ab/depth
            k4a::capture combined_cap = k4a::capture::create();

            if (input_config.color_track_enabled)
            {
                combined_cap.set_color_image(input_capture.get_color_image());
            }
            int stride_bytes = (int)outputCaptureInfo.output_width * (int)sizeof(uint16_t);
            std::chrono::microseconds us(K4A_90K_HZ_TICK_TO_USEC(outputCaptureInfo.center_of_exposure_in_ticks));

            if (input_config.depth_mode == K4A_DEPTH_MODE_PASSIVE_IR)
            {
                k4a::image ir_img = k4a::image::create_from_buffer(K4A_IMAGE_FORMAT_IR16,
                                                                   outputCaptureInfo.output_width,
                                                                   outputCaptureInfo.output_height,
                                                                   stride_bytes,
                                                                   output_buf.data(),
                                                                   (size_t)stride_bytes *
                                                                       (size_t)outputCaptureInfo.output_height,
                                                                   nullptr,
                                                                   nullptr);

                ir_img.set_timestamp(us);
                combined_cap.set_ir_image(ir_img);
            }
            else
            {
                k4a::image depth_img = k4a::image::create_from_buffer(K4A_IMAGE_FORMAT_DEPTH16,
                                                                      outputCaptureInfo.output_width,
                                                                      outputCaptureInfo.output_height,
                                                                      stride_bytes,
                                                                      output_buf.data(),
                                                                      (size_t)stride_bytes *
                                                                          (size_t)outputCaptureInfo.output_height,
                                                                      nullptr,
                                                                      nullptr);

                k4a::image ir_img =
                    k4a::image::create_from_buffer(K4A_IMAGE_FORMAT_IR16,
                                                   outputCaptureInfo.output_width,
                                                   outputCaptureInfo.output_height,
                                                   stride_bytes,
                                                   output_buf.data() + stride_bytes * outputCaptureInfo.output_height,
                                                   (size_t)stride_bytes * (size_t)outputCaptureInfo.output_height,
                                                   nullptr,
                                                   nullptr);

                depth_img.set_timestamp(us);
                ir_img.set_timestamp(us);

                combined_cap.set_depth_image(depth_img);
                combined_cap.set_ir_image(ir_img);
            }

            recorder.write_capture(combined_cap);
        }
        else
        {
            std::cout << "Depth Engine Processing error. Error code: " << result << std::endl;
            return 1;
        }

        nframes++;
    }

    input.close();
    recorder.flush();
    std::cout << std::endl << std::endl;
    std::cout << "Total Frames Processed: " << nframes << std::endl;
    std::cout << "Average Processing Time Per Frame: " << (double)process_time_ms / nframes << "ms" << std::endl;

    return 0;
}