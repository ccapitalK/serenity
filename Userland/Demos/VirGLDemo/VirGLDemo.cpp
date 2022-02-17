/*
 * Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/ioctl_numbers.h>
#include <unistd.h>

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"

int gpu_fd;
u32 vbo_resource_id;

static void upload_command_buffer(Vector<u32> const& command_buffer) {
    VirGLCommandBuffer command_buffer_descriptor {
        .data = command_buffer.data(),
        .num_elems = command_buffer.size(),
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SUBMIT_CMD, &command_buffer_descriptor) >= 0);
}

static void init() {
    // Open the device
    gpu_fd = open("/dev/gpu0", O_RDWR);
    VERIFY(gpu_fd >= 0);
    // Do kernel space setup (disable writes from display server)
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SETUP_DEMO) >= 0);
    // Create a VertexElements resource
    VirGL3DResourceSpec vbo_spec {
        .target = AK::to_underlying(Gallium::PipeTextureTarget::BUFFER), // pipe_texture_target
        .format = 45, // pipe_to_virgl_format
        .bind = VIRGL_BIND_VERTEX_BUFFER,
        .width = PAGE_SIZE,
        .height = 1,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0,
        .created_resource_id = 0,
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_CREATE_RESOURCE, &vbo_spec) >= 0);
    vbo_resource_id = vbo_spec.created_resource_id;
    dbgln("Got vbo id: {}", vbo_resource_id);
    // Initialize all required state
    CommandBufferBuilder builder;
    // Set the vertex buffer
    builder.append_set_vertex_buffers(20, 0, vbo_resource_id);
    upload_command_buffer(builder.build());
}

struct VertexData {
    float r;
    float g;
    float b;
    float x;
    float y;
};

static VertexData gen_rand_colored_vertex_at(float x, float y) {
    return {
        .r = ((float)(rand() % 256)) / 255.f,
        .g = ((float)(rand() % 256)) / 255.f,
        .b = ((float)(rand() % 256)) / 255.f,
        .x = x,
        .y = y,
    };
}

static void draw_frame() {
    float top_x_ordinate = 0.9 - ((rand() % 18) / 10.0);
    VertexData vertices[3] = {
        gen_rand_colored_vertex_at(-0.8, -0.8),
        gen_rand_colored_vertex_at(0.8, -0.8),
        gen_rand_colored_vertex_at(top_x_ordinate, 0.9),
    };
    VirGLTransferDescriptor descriptor {
        .data = (void*)vertices,
        .offset_in_region = 0,
        .num_bytes = sizeof(vertices),
        .direction = VIRGL_DATA_DIR_GUEST_TO_HOST,
    };
    dbgln("Going to transfer vertex data");
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_TRANSFER_DATA, &descriptor) >= 0);
    dbgln("Going to draw");
    // Create command buffer
    CommandBufferBuilder builder;
    // Transfer data to vbo
    builder.append_transfer3d_flat(vbo_resource_id, sizeof(vertices));
    builder.append_end_transfers_3d();
    // Clear the framebuffer
    builder.append_gl_clear(0, 0, 0);
    // Draw the vbo
    builder.append_draw_vbo(3);
    upload_command_buffer(builder.build());
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_FLUSH_DISPLAY) >= 0);
}

static void finish() {
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_FINISH_DEMO) >= 0);
}

ErrorOr<int> serenity_main(Main::Arguments)
{
    init();
    for (int i = 0; i < 40; ++i) {
        draw_frame();
        usleep(200000);
    }
    finish();
    return {0};
}
