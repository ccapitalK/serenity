/*
 * Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGUI/Application.h>
#include <LibGUI/Icon.h>
#include <LibGUI/Window.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/ioctl_numbers.h>
#include <unistd.h>

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"
#include "Widget.h"

static const char* frag_shader = "FRAG\n"
                                 "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
                                 "DCL IN[0], COLOR, COLOR\n"
                                 "DCL OUT[0], COLOR\n"
                                 "  0: MOV OUT[0], IN[0]\n"
                                 "  1: END\n";

static const char* vert_shader = "VERT\n"
                                 "DCL IN[0]\n"
                                 "DCL IN[1]\n"
                                 "DCL OUT[0], POSITION\n"
                                 "DCL OUT[1], COLOR\n"
                                 "DCL CONST[0..3]\n"
                                 "DCL TEMP[0..1]\n"
                                 "  0: MUL TEMP[0], IN[0].xxxx, CONST[0]\n"
                                 "  1: MAD TEMP[1], IN[0].yyyy, CONST[1], TEMP[0]\n"
                                 "  2: MAD TEMP[0], IN[0].zzzz, CONST[2], TEMP[1]\n"
                                 "  3: MAD OUT[0], IN[0].wwww, CONST[3], TEMP[0]\n"
                                 "  4: MOV_SAT OUT[1], IN[1]\n"
                                 "  5: END\n";

int gpu_fd;
ResourceID vbo_resource_id;
ResourceID drawtarget;
ObjectHandle blend_handle;
ObjectHandle drawtarget_surface_handle;
ObjectHandle ve_handle;
ObjectHandle frag_shader_handle;
ObjectHandle vert_shader_handle;

static ObjectHandle allocate_handle()
{
    // FIXME: We should instead be creating a VirtIOGPU context per process
    // Set to initially 32, to prevent collisions with resources created by the kernel
    static u32 last_allocated_handle = 32;
    return { ++last_allocated_handle };
}

static void upload_command_buffer(Vector<u32> const& command_buffer)
{
    VERIFY(command_buffer.size() <= AK::NumericLimits<u32>::max());
    VirGLCommandBuffer command_buffer_descriptor {
        .data = command_buffer.data(),
        .num_elems = (u32)command_buffer.size(),
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SUBMIT_CMD, &command_buffer_descriptor) >= 0);
}

static ResourceID create_virgl_resource(VirGL3DResourceSpec& spec)
{
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_CREATE_RESOURCE, &spec) >= 0);
    return spec.created_resource_id;
}

static void init()
{
    // Open the device
    gpu_fd = open("/dev/gpu0", O_RDWR);
    VERIFY(gpu_fd >= 0);
    // Create a VertexElements resource
    VirGL3DResourceSpec vbo_spec {
        .target = AK::to_underlying(Gallium::PipeTextureTarget::BUFFER), // pipe_texture_target
        .format = 45,                                                    // pipe_to_virgl_format
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
    vbo_resource_id = create_virgl_resource(vbo_spec);
    // Create a texture to draw to
    VirGL3DResourceSpec drawtarget_spec {
        .target = AK::to_underlying(Gallium::PipeTextureTarget::TEXTURE_RECT),                  // pipe_texture_target
        .format = AK::to_underlying(Protocol::TextureFormat::VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM), // pipe_to_virgl_format
        .bind = VIRGL_BIND_RENDER_TARGET,
        .width = DRAWTARGET_WIDTH,
        .height = DRAWTARGET_HEIGHT,
        .depth = 1,
        .array_size = 1,
        .last_level = 0,
        .nr_samples = 0,
        .flags = 0,
        .created_resource_id = 0,
    };
    drawtarget = create_virgl_resource(drawtarget_spec);

    // Initialize all required state
    CommandBufferBuilder builder;
    // Create and set the blend, to control the color mask
    blend_handle = allocate_handle();
    builder.append_create_blend(blend_handle);
    builder.append_bind_blend(blend_handle);
    // Create drawtarget surface
    drawtarget_surface_handle = allocate_handle();
    builder.append_create_surface(drawtarget, drawtarget_surface_handle);
    // Set some framebuffer state (attached handle, framebuffer size, etc)
    builder.append_set_framebuffer_state(drawtarget_surface_handle);
    builder.append_set_framebuffer_state_no_attach();
    // Set the vertex buffer
    builder.append_set_vertex_buffers(20, 0, vbo_resource_id);
    // Create and bind fragment shader
    frag_shader_handle = allocate_handle();
    builder.append_create_shader(frag_shader_handle, Gallium::ShaderType::SHADER_FRAGMENT, frag_shader);
    builder.append_bind_shader(frag_shader_handle, Gallium::ShaderType::SHADER_FRAGMENT);
    // Create and bind vertex shader
    vert_shader_handle = allocate_handle();
    builder.append_create_shader(vert_shader_handle, Gallium::ShaderType::SHADER_VERTEX, vert_shader);
    builder.append_bind_shader(vert_shader_handle, Gallium::ShaderType::SHADER_VERTEX);
    // Create a VertexElements object (used to specify layout of vertex data)
    ve_handle = allocate_handle();
    builder.append_create_vertex_elements(ve_handle);
    builder.append_bind_vertex_elements(ve_handle);
    // Set the Viewport
    builder.append_gl_viewport();
    // FIXME: Changing the identity matrix to fix display orientation is bad practice, we should instead
    //        find a proper way of flipping the Y coordinates
    // Set the constant buffer to the identity matrix (negate the y multiplicand, since the drawn texture
    // would otherwise be upside down relative to serenity's bitmap encoding)
    builder.append_set_constant_buffer({ 1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0 });
    // Upload buffer
    upload_command_buffer(builder.build());
}

struct VertexData {
    float r;
    float g;
    float b;
    float x;
    float y;
};

static VertexData gen_rand_colored_vertex_at(float x, float y)
{
    return {
        .r = ((float)(rand() % 256)) / 255.f,
        .g = ((float)(rand() % 256)) / 255.f,
        .b = ((float)(rand() % 256)) / 255.f,
        .x = x,
        .y = y,
    };
}

static void draw_frame()
{
    // Choose random top vertex x ordinate
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
    // Transfer data from vertices array to kernel virgl transfer region
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_TRANSFER_DATA, &descriptor) >= 0);
    // Create command buffer
    CommandBufferBuilder builder;
    // Transfer data from kernel virgl transfer region to host resource
    builder.append_transfer3d(vbo_resource_id, sizeof(vertices), 1, 1, VIRGL_DATA_DIR_GUEST_TO_HOST);
    builder.append_end_transfers_3d();
    // Clear the framebuffer
    builder.append_gl_clear(0, 0, 0);
    // Draw the vbo
    builder.append_draw_vbo(3);
    // Upload the buffer
    upload_command_buffer(builder.build());
}

void update_frame(RefPtr<Gfx::Bitmap> target)
{
    VERIFY(target->width() == DRAWTARGET_WIDTH);
    VERIFY(target->height() == DRAWTARGET_HEIGHT);
    // Run logic to draw the frame
    draw_frame();
    // Transfer data back from hypervisor to kernel transfer region
    CommandBufferBuilder builder;
    builder.append_transfer3d(drawtarget, DRAWTARGET_WIDTH, DRAWTARGET_HEIGHT, 1, VIRGL_DATA_DIR_HOST_TO_GUEST);
    builder.append_end_transfers_3d();
    upload_command_buffer(builder.build());
    // Copy from kernel transfer region to userspace
    VirGLTransferDescriptor descriptor {
        .data = (void*)target->scanline_u8(0),
        .offset_in_region = 0,
        .num_bytes = DRAWTARGET_WIDTH * DRAWTARGET_HEIGHT * sizeof(u32),
        .direction = VIRGL_DATA_DIR_HOST_TO_GUEST,
    };
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_TRANSFER_DATA, &descriptor) >= 0);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    auto app = TRY(GUI::Application::try_create(arguments));

    auto window = TRY(GUI::Window::try_create());
    window->set_double_buffering_enabled(true);
    window->set_title("VirGLDemo");
    window->set_resizable(false);
    window->resize(DRAWTARGET_WIDTH, DRAWTARGET_HEIGHT);
    window->set_has_alpha_channel(true);
    window->set_alpha_hit_threshold(1);

    auto demo = TRY(window->try_set_main_widget<Demo>());

    auto app_icon = GUI::Icon::default_icon("app-cube");
    window->set_icon(app_icon.bitmap_for_size(16));

    init();
    window->show();

    return app->exec();
}
