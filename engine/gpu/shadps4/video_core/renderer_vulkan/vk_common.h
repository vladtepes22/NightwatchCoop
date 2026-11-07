// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Include vulkan-hpp header
#define VK_ENABLE_BETA_EXTENSIONS
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#define VULKAN_HPP_HAS_SPACESHIP_OPERATOR
#define VULKAN_HPP_NO_EXCEPTIONS
// Define assert-on-result to nothing to instead return the result for our handling.
#define VULKAN_HPP_ASSERT_ON_RESULT

// bbport: Vulkan-Hpp's DispatchLoaderBase has two more members unless NDEBUG is defined, so every
// unit must see it the way bbgpu does (NDEBUG). Tests build with -UNDEBUG for assert: one that
// instantiated a dispatcher inline (DispatchLoaderDynamic::init) gave the linker a second layout,
// and the copy it kept read vkGetInstanceProcAddr 16 bytes off, a null call at start (the
// scene-resolution, taa-shader and camera-motion tests, 7 Nov). assert comes back afterwards.
#ifndef NDEBUG
#define BBPORT_VK_RESTORE_ASSERT
#define NDEBUG
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value"
#include <vulkan/vulkan.hpp>
#pragma clang diagnostic pop
#ifdef BBPORT_VK_RESTORE_ASSERT
#undef BBPORT_VK_RESTORE_ASSERT
#undef NDEBUG
#include <assert.h> // no include guard: assert is defined again, now checking
#endif

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
