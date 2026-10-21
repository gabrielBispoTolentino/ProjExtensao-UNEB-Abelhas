#pragma once

// Supabase dos testes: o servidor falso de tst_cloudsync (fakehttpserver.h),
// sempre na mesma porta, porque o endereço é uma constante de compilação.
namespace SupabaseConfig {
constexpr auto url = "http://127.0.0.1:18767";
constexpr auto key = "sb_publishable_test";
constexpr unsigned short testPort = 18767;
} // namespace SupabaseConfig
