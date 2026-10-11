#pragma once
#include <klib/types.hpp>

// Log levels. The threshold is settable from the boot cmdline (loglevel=...).
enum class LogLevel : u8 { Error = 0, Warn = 1, Info = 2, Debug = 3 };

void klog_set_level(LogLevel level);
bool klog_enabled(LogLevel level);

// Minimal printf. Supported: %c %s %d %i %u %x %X %p %%
//   flags   '-' left-align, '0' zero-pad
//   width   decimal, or '*'
//   length  'l', 'll', 'z'
usize kvsnprintf(char* buf, usize size, const char* fmt, va_list args);
usize ksnprintf(char* buf, usize size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

void kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void klog(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_ERROR(...) klog(LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...)  klog(LogLevel::Warn,  __VA_ARGS__)
#define LOG_INFO(...)  klog(LogLevel::Info,  __VA_ARGS__)
#define LOG_DEBUG(...) klog(LogLevel::Debug, __VA_ARGS__)
