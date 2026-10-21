-- BeeDuino - estrutura do banco no Supabase.
-- Rode uma vez no painel do Supabase: SQL Editor -> New query -> colar -> Run.
-- Pode rodar de novo sem problema (tudo é "if not exists" / substituído).
--
-- Uma linha por *registro*: cada ESP32 guarda a média das leituras de um
-- intervalo (1 minuto por padrão). Os registros chegam a esta tabela de dois jeitos:
--   * a própria ESP32 envia quando tem Wi-Fi (source = 'esp32');
--   * o app envia os registros que recebeu por Bluetooth enquanto a placa
--     estava sem internet (source = 'app').
-- Um registro é identificado por (device_id, boot, seq), então o mesmo
-- registro chegando pelos dois caminhos - ou do celular e do PC - é guardado
-- uma vez só (os envios usam "on conflict do nothing").

create table if not exists public.readings (
    id          bigint generated always as identity primary key,
    device_id   text        not null,            -- MAC da ESP32 sem ":"
    device_name text,                            -- nome amigável definido no app
    recorded_at timestamptz not null,            -- quando a placa mediu
    temperature real        not null,            -- °C
    humidity    real,                            -- %
    boot        integer     not null,            -- contador de inicializações da placa
    seq         integer     not null,            -- número do registro dentro dessa inicialização
    source      text        not null default 'esp32' check (source in ('esp32', 'app')),
    inserted_at timestamptz not null default now(),
    constraint readings_unique_record unique (device_id, boot, seq)
);

-- Os apps sincronizam linhas novas pelo id (chave primária) e conferem de
-- novo as inseridas nos últimos ~2 minutos (inserted_at), caso dois envios
-- tenham aparecido fora da ordem de id. (Uma versão antiga deste arquivo tinha
-- um índice em (device_id, recorded_at) que nada usava; ele é removido.)
drop index if exists public.readings_device_time;
create index if not exists readings_inserted_at on public.readings (inserted_at);

-- Acesso com a chave pública do projeto (anon / publishable), que vai no
-- firmware e no app: quem tem a chave pode ler e adicionar leituras, mas
-- nunca alterá-las, e só apaga com a senha de administrador (abaixo).
alter table public.readings enable row level security;

drop policy if exists "readings are public" on public.readings;
create policy "readings are public"
    on public.readings for select
    to anon
    using (true);

drop policy if exists "anyone can add readings" on public.readings;
create policy "anyone can add readings"
    on public.readings for insert
    to anon
    with check (true);

grant select, insert on public.readings to anon;

-- ── Apagar leituras pelo app ──────────────────────────────────────────────
-- A chave pública não apaga nada. A tela "Limpar dados" do app chama a função
-- delete_readings() abaixo, que só apaga quando recebe a senha de
-- administrador, guardada aqui como hash bcrypt num schema que a API não expõe.
--
-- Defina (ou troque) a senha uma vez, no SQL Editor - use uma senha longa,
-- pois quem tem a chave pública pode tentar adivinhá-la:
--
--   insert into private.admin_password (password_hash)
--   values (extensions.crypt('SUA-SENHA', extensions.gen_salt('bf', 10)))
--   on conflict (id) do update set password_hash = excluded.password_hash;

create extension if not exists pgcrypto with schema extensions;

create schema if not exists private;
revoke all on schema private from public, anon, authenticated;

create table if not exists private.admin_password (
    id            boolean primary key default true check (id),   -- uma única linha
    password_hash text not null
);
revoke all on private.admin_password from public, anon, authenticated;

-- Apaga até max_rows leituras de uma placa (todas, se target_device_id for
-- null) gravadas antes de recorded_before (qualquer data, se null) e devolve
-- quantas apagou. O app chama de novo até voltarem menos que max_rows: pedidos
-- com a chave pública expiram em poucos segundos, pouco para apagar um ano de
-- leituras de uma vez.
create or replace function public.delete_readings(
    admin_password   text,
    target_device_id text        default null,
    recorded_before  timestamptz default null,
    max_rows         integer     default 10000)
returns bigint
language plpgsql
security definer
set search_path = ''
as $$
declare
    stored_hash text;
    deleted     bigint;
begin
    select password_hash into stored_hash from private.admin_password;
    if stored_hash is null then
        raise exception 'admin password not set' using errcode = '28000';
    end if;
    if extensions.crypt(coalesce(admin_password, ''), stored_hash) <> stored_hash then
        perform pg_sleep(1);   -- atrasa tentativas de adivinhar
        raise exception 'wrong admin password' using errcode = '28P01';
    end if;

    delete from public.readings
     where id in (select id
                    from public.readings
                   where (target_device_id is null or device_id = target_device_id)
                     and (recorded_before is null or recorded_at < recorded_before)
                   order by id
                   limit greatest(1, least(max_rows, 50000)));
    get diagnostics deleted = row_count;
    return deleted;
end;
$$;

revoke all on function public.delete_readings(text, text, timestamptz, integer) from public, authenticated;
grant execute on function public.delete_readings(text, text, timestamptz, integer) to anon;

-- Faz a API enxergar a função nova na hora.
notify pgrst, 'reload schema';
