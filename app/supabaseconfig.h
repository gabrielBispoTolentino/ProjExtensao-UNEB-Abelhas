#pragma once

// Projeto Supabase usado pelo app e pelo firmware (mesmos valores de
// SUPABASE_URL / SUPABASE_KEY em firmware/ColmeiaMonitor/ColmeiaMonitor.ino).
// A tabela é criada por supabase/schema.sql.
//
// Use a chave pública do projeto (anon ou sb_publishable_...), nunca a
// service_role / secreta: esta chave vai dentro do app.
namespace SupabaseConfig {

constexpr auto url = "https://ysepuwpswahtobpatarh.supabase.co";
constexpr auto key = "sb_publishable_LOlfANl1hGM5-YZ_mTqaWQ_6FuXoHk1";

} // namespace SupabaseConfig
