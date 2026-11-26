-- kmod built-in mod loader (runs as a built-in script, owner ""). Implements the mod pack format v1/v2:
--   kmod/mods/<id>/{mod.json, main.lua, lang/<tag>.json}   mod folders AND .kmod packages dropped by the user (shared storage)
--   <private>/mods/<id>/                                     what is actually loaded: kmod.sync_mods mirrors kmod/mods here
--                                                            (folders copied, .kmod extracted); shared storage is not executable
--   kmod/data/<id>/settings.json                           user settings
--   kmod/state.json                                        {"enabled":[ids], "language":"auto"}
-- Public (kmod.*): api, json, paths, state helpers, scan_mods, load_mods, sync_mods, set_enabled, settings_get/set,
-- lang_current, game_language, rec_text, mod_title, setting_label, option_label, t.

local P = kmod.paths
P.root = P.root or (P.files .. "/kmod")        -- set natively: the shared media folder
P.data = P.root .. "/data"
P.loaded = P.private .. "/mods"                -- the private copy that mods are loaded from
P.state = P.root .. "/state.json"

local function log(...) kmod.log("info", "loader", ...) end
local function logw(...) kmod.log("warn", "loader", ...) end
local function loge(...) kmod.log("error", "loader", ...) end

local json = kmod.json   -- json.lua is run by the host before this script

-- ------------------------------------------------------------------ files
local function readFile(p) local f = io.open(p, "rb"); if not f then return nil end local t = f:read("a"); f:close(); return t end
local function writeFile(p, t) local f = io.open(p, "wb"); if not f then return false end f:write(t); f:close(); return true end
local function mkdirs(path)
  local acc = path:match("^%a:") or ""                  -- Windows drive prefix
  for part in path:gsub("^%a:", ""):gmatch("[^/]+") do
    acc = acc .. "/" .. part
    if not kmod.exists(acc) then kmod.mkdir(acc) end
  end
end
kmod.readFile, kmod.writeFile, kmod.mkdirs = readFile, writeFile, mkdirs

-- ------------------------------------------------------------------ state.json
local state = { enabled = {}, language = "auto" }
kmod.state = state

local function loadState()
  local t = json.decode(readFile(P.state) or "{}") or {}
  state.language = type(t.language) == "string" and t.language or "auto"
  state.enabled = {}
  for _, id in ipairs(t.enabled or {}) do state.enabled[id] = true end
end

local function saveState()
  local list = {}
  for id in pairs(state.enabled) do list[#list + 1] = id end
  table.sort(list)
  writeFile(P.state, json.encode({ enabled = list, language = state.language }))
end

function kmod.set_enabled(id, on)
  loadState()                                           -- always start from what is on disk
  state.enabled[id] = on and true or nil
  if on then
    if kmod.auto_disabled then kmod.auto_disabled[id] = nil end    -- re-enabling restarts the error count
    if kmod._errCount then kmod._errCount[id] = nil end
    if kmod.unquarantine then kmod.unquarantine("mod", id) end     -- the user decided to try it again after a crash
  end
  saveState()
end

-- ------------------------------------------------------------------ language + localization
local TAGS = { "en", "zh-CN", "zh-TW", "ja", "ko", "ru", "de", "fr", "es", "pt", "pl", "it" }
kmod.language_tags = TAGS

local function normalizeTag(culture)
  if not culture or culture == "" then return nil end
  local lower = culture:lower():gsub("_", "-")
  if lower:sub(1, 2) == "zh" then
    if lower:find("hant") or lower:find("tw") or lower:find("hk") or lower:find("mo") then return "zh-TW" end
    return "zh-CN"
  end
  local base = lower:match("^(%a+)")
  for _, t in ipairs(TAGS) do if t:lower() == base then return t end end
  return nil
end

function kmod.game_language()
  local ok, culture = pcall(function()
    local lib = StaticFindObject("/Script/Engine.Default__KismetInternationalizationLibrary")
    return lib:GetCurrentLanguage()
  end)
  return ok and normalizeTag(culture) or nil
end

function kmod.lang_current()
  if state.language ~= "auto" then return state.language end
  return kmod.game_language() or "en"
end

local function langChain(tag)
  local chain = { tag }
  local base = tag:match("^(%a+)-")
  if base then chain[#chain + 1] = base end
  if tag ~= "en" then chain[#chain + 1] = "en" end
  return chain
end

local function langTable(rec, tag)
  rec._lang = rec._lang or {}
  if rec._lang[tag] == nil then
    local txt = readFile(rec.dir .. "/lang/" .. tag .. ".json")
    rec._lang[tag] = txt and json.decode(txt) or false
  end
  return rec._lang[tag]
end

function kmod.rec_text(rec, key)                      -- translated text for a mod key, or nil
  for _, tag in ipairs(langChain(kmod.lang_current())) do
    local t = langTable(rec, tag)
    if t and t[key] ~= nil then return tostring(t[key]) end
  end
  return nil
end

local function interpolate(s, vars)
  return (s:gsub("{([%w_]+)}", function(k) local v = vars and vars[k]; if v ~= nil then return tostring(v) end return "{" .. k .. "}" end))
end

function kmod.mod_title(rec) return kmod.rec_text(rec, "name") or rec.name end
function kmod.mod_description(rec) return kmod.rec_text(rec, "description") or rec.description or "" end
function kmod.setting_label(rec, s) return kmod.rec_text(rec, "settings." .. s.id .. ".label") or s.label or s.id end
function kmod.option_label(rec, s, value) return kmod.rec_text(rec, "settings." .. s.id .. ".options." .. tostring(value)) or tostring(value) end

-- strings of the manager UI itself
local UI = {
  en = { mods = "Mods", version = "Version", enabled = "Enabled", language = "Language", status = "Status",
         no_mods = "No mods found", no_mods_hint = "Put mod folders or .kmod files in kmod/mods", auto = "Auto (game language)", reload = "Reload", reload_all = "Reload all mods",
         pak_pending = "Pak update waiting", pak_confirm = "Install", pak_write = "Writes", pak_remove = "Removes", pak_conflict = "Overwrites files it did not install", pak_restart = "Pak changes take effect after restarting the game." },
  ["zh-CN"] = { mods = "模组", version = "版本", enabled = "启用", language = "语言", status = "状态",
                no_mods = "未找到模组", no_mods_hint = "把模组文件夹或 .kmod 文件放进 kmod/mods", auto = "自动（跟随游戏）", reload = "重载", reload_all = "重载所有模组",
                pak_pending = "Pak 更新待确认", pak_confirm = "安装", pak_write = "写入", pak_remove = "删除", pak_conflict = "将覆盖不是它安装的文件", pak_restart = "Pak 改动在重启游戏后生效。" },
  ["zh-TW"] = { mods = "模組", version = "版本", enabled = "啟用", language = "語言", status = "狀態",
                no_mods = "找不到模組", no_mods_hint = "把模組資料夾或 .kmod 檔案放進 kmod/mods", auto = "自動（跟隨遊戲）", reload = "重載", reload_all = "重載所有模組",
                pak_pending = "Pak 更新待確認", pak_confirm = "安裝", pak_write = "寫入", pak_remove = "刪除", pak_conflict = "將覆蓋不是它安裝的檔案", pak_restart = "Pak 變更在重新啟動遊戲後生效。" },
}
function kmod.t(key)
  for _, tag in ipairs(langChain(kmod.lang_current())) do
    local t = UI[tag]
    if t and t[key] then return t[key] end
  end
  return key
end
kmod.language_names = { auto = "auto", en = "English", ["zh-CN"] = "简体中文", ["zh-TW"] = "繁體中文", ja = "日本語", ko = "한국어",
                       ru = "Русский", de = "Deutsch", fr = "Français", es = "Español", pt = "Português", pl = "Polski", it = "Italiano" }

function kmod.set_language(tag)
  loadState()
  state.language = tag
  saveState()
  kmod._notify_language()
end

-- ------------------------------------------------------------------ settings values
local function loadValues(rec)
  rec.values = json.decode(readFile(P.data .. "/" .. rec.id .. "/settings.json") or "{}") or {}
  rec.defaults = {}
  rec.settingById = {}
  for _, s in ipairs(rec.settings) do rec.defaults[s.id] = s.default; rec.settingById[s.id] = s end
end

function kmod.settings_get(rec, id)
  local v = rec.values[id]
  local s = rec.settingById[id]
  if v ~= nil and s and s.type == "choice" then
    local ok = false
    for _, o in ipairs(s.options or {}) do if o == v then ok = true end end
    if not ok then v = nil end                          -- stored value is not an option any more -> default
  end
  if v ~= nil and s then                                -- hand-edited / stale values of the wrong type -> default
    local want = ({ bool = "boolean", number = "number", string = "string" })[s.type]
    if want and type(v) ~= want then v = nil end
  end
  if v == nil then v = rec.defaults[id] end
  if v == nil and s and s.type == "string" then v = "" end
  return v
end

function kmod.settings_set(rec, id, v, silent)
  -- the UI may hold a record from before the last reload; the live one (with the mod's handlers) is looked up by id
  local live = (kmod._recById and kmod._recById[rec.id]) or rec
  live.values[id] = v
  if rec ~= live then rec.values[id] = v end
  mkdirs(P.data .. "/" .. rec.id)
  writeFile(P.data .. "/" .. rec.id .. "/settings.json", json.encode(live.values))
  if not silent then
    for _, fn in ipairs(live._handlers or {}) do
      local ok, err = pcall(fn, id, v)
      if not ok then loge(rec.id .. " Settings.on_change error: " .. tostring(err)) end
    end
  end
end

-- ------------------------------------------------------------------ manifest + scanning
local ID_PAT = "^[a-z0-9_]+$"
kmod._status = kmod._status or {}                         -- id -> last status message of the last load

-- Dependency specs: "id", "id>=1.2.0", "id>=1.0.0,<2.0.0". "kmod" is the loader itself.
local function parseVer(s)
  local a, b, c = tostring(s):match("^%s*v?(%d+)%.?(%d*)%.?(%d*)")
  if not a then return nil end
  return { tonumber(a), tonumber(b) or 0, tonumber(c) or 0 }
end
local function cmpVer(x, y)
  for i = 1, 3 do if x[i] ~= y[i] then return x[i] < y[i] and -1 or 1 end end
  return 0
end
local function parseDep(spec)
  if type(spec) ~= "string" then return nil, "not a string" end
  local id, rest = spec:match("^%s*([a-z0-9_]+)%s*(.-)%s*$")
  if not id then return nil, "bad id" end
  local cons = {}
  if rest ~= "" then
    for part in rest:gmatch("[^,]+") do
      local op, v = part:match("^%s*([<>=]=?)%s*(.-)%s*$")
      local pv = v and parseVer(v)
      if not op or not pv or not (op == ">=" or op == ">" or op == "<=" or op == "<" or op == "=" or op == "==") then
        return nil, "bad version condition '" .. part:match("^%s*(.-)%s*$") .. "'"
      end
      cons[#cons + 1] = { op = op, ver = pv, text = op .. v }
    end
  end
  return id, cons
end
-- returns ok, description of the first failing condition
local function versionOk(have, cons)
  local hv = parseVer(have)
  for _, c in ipairs(cons) do
    if not hv then return false, c.text end
    local r = cmpVer(hv, c.ver)
    local ok = (c.op == ">=" and r >= 0) or (c.op == ">" and r > 0) or (c.op == "<=" and r <= 0) or (c.op == "<" and r < 0) or ((c.op == "=" or c.op == "==") and r == 0)
    if not ok then return false, c.text end
  end
  return true
end
kmod.version_satisfies = function(have, spec)
  local _, cons = parseDep("x" .. (spec or ""))
  if type(cons) ~= "table" then return nil, cons end
  return versionOk(have, cons)
end

-- Reserved dependency ids: the loader and the game itself. Unknown game version => the condition is skipped with a warning.
local function reservedVersion(id)
  if id == "kmod" then return kmod.version end
  if id == "kards" then return kmod.game end
end
local RESERVED = { kmod = true, kards = true }

-- The game version comes from GeneralProjectSettings.ProjectVersion ("Kards 1.61.27596.APK"); only major.minor is used ("1.61").
function kmod.detect_game_version()
  local ok, v = pcall(function()
    local g = StaticFindObject("/Script/EngineSettings.Default__GeneralProjectSettings")
    return g and g.ProjectVersion
  end)
  if not ok or type(v) ~= "string" then return nil end
  local full = v:match("%d+%.%d+%.%d+") or v:match("%d+%.%d+")
  local short = v:match("(%d+%.%d+)")
  if not short then return nil end
  kmod.game_version, kmod.game = full, short
  return short
end

-- mod.json "platform": "Android" | "Windows" | ["Windows", "Android"]. No field = every platform (a pure Lua mod runs anywhere);
-- a mod with native code must declare the platforms it was built for. Needs "format": 2 so a loader that does not know the
-- field refuses the package instead of running it on the wrong platform.
kmod.platform = kmod.platform or "Android"                -- the Windows core sets "Windows" before the loader runs
local KNOWN_PLATFORMS = { windows = "Windows", android = "Android" }
local function readPlatforms(def, rec)
  local v = def.platform
  if v == nil then return nil end
  if type(v) == "string" then v = { v } end
  if type(v) ~= "table" then rec.errors[#rec.errors + 1] = "platform must be a string or a list"; return nil end
  local set, any, names = {}, false, {}
  for _, p in ipairs(v) do
    local k = KNOWN_PLATFORMS[tostring(p):lower()]
    if k then if not set[k] then set[k] = true; names[#names + 1] = k end; any = true
    else rec.warnings[#rec.warnings + 1] = "unknown platform '" .. tostring(p) .. "' ignored" end
  end
  if not any then rec.errors[#rec.errors + 1] = "platform lists no known platform (Windows, Android)"; return nil end
  rec.platforms = names
  return set
end

local function inspect(folder)
  local rec = { folder = folder, dir = P.loaded .. "/" .. folder, errors = {}, warnings = {} }
  local def = {}
  local txt = readFile(rec.dir .. "/mod.json")
  if not txt then rec.errors[#rec.errors + 1] = "mod.json missing"
  else
    local d, perr = json.decode(txt)
    if type(d) ~= "table" then rec.errors[#rec.errors + 1] = "bad mod.json (" .. tostring(perr) .. ")" else def = d end
  end
  rec.def = def
  rec.id = def.id or folder
  if def.id == nil then rec.warnings[#rec.warnings + 1] = "mod.json has no id (using folder name)" end
  if type(rec.id) ~= "string" or not rec.id:match(ID_PAT) then rec.errors[#rec.errors + 1] = "invalid id '" .. tostring(rec.id) .. "'"
  elseif rec.id ~= folder then rec.errors[#rec.errors + 1] = "id '" .. rec.id .. "' does not match folder '" .. folder .. "'" end
  if def.format == nil then rec.warnings[#rec.warnings + 1] = "no format field (assuming 1)"
  elseif def.format ~= 1 and def.format ~= 2 then rec.errors[#rec.errors + 1] = "unsupported format " .. tostring(def.format) end
  local platforms = readPlatforms(def, rec)
  if platforms and def.format ~= 2 then rec.warnings[#rec.warnings + 1] = "platform needs \"format\": 2 (older loaders would ignore it)" end
  if platforms and not platforms[kmod.platform] then
    rec.unsupported = "not for this platform (" .. table.concat(rec.platforms, ", ") .. " only; this is " .. kmod.platform .. ")"
    rec.errors[#rec.errors + 1] = rec.unsupported
  end
  rec.version = def.version and tostring(def.version) or ""
  if def.version == nil then rec.warnings[#rec.warnings + 1] = "no version" end
  rec.name = def.name or rec.id
  rec.author, rec.description = def.author or "", def.description or ""
  if type(def.api) == "number" and def.api > kmod.api then rec.errors[#rec.errors + 1] = "needs loader API " .. tostring(def.api) .. " (have " .. kmod.api .. ")" end
  rec.permissions = {}                                    -- declared in mod.json "permissions": ["network"]; shown to the user, see docs
  if def.permissions ~= nil and type(def.permissions) ~= "table" then rec.errors[#rec.errors + 1] = "permissions must be a list" end
  for _, p in ipairs(type(def.permissions) == "table" and def.permissions or {}) do
    if p == "network" or p == "game_network" then rec.permissions[p] = true
    else rec.warnings[#rec.warnings + 1] = "unknown permission '" .. tostring(p) .. "' ignored" end
  end
  rec.depends, rec.depCons = {}, {}                       -- depends = mod ids to load first; depCons[id] = version conditions
  if def.depends ~= nil and type(def.depends) ~= "table" then rec.errors[#rec.errors + 1] = "depends must be a list" end
  for _, spec in ipairs(type(def.depends) == "table" and def.depends or {}) do
    local id, cons = parseDep(spec)
    if not id then rec.errors[#rec.errors + 1] = "bad dependency " .. tostring(spec) .. " (" .. tostring(cons) .. ")"
    elseif RESERVED[id] then
      local have = reservedVersion(id)
      if not have then
        if #cons > 0 then rec.warnings[#rec.warnings + 1] = id .. " version unknown, condition ignored" end
      else
        local ok, bad = versionOk(have, cons)
        if not ok then rec.errors[#rec.errors + 1] = "requires " .. id .. bad .. " (have " .. have .. ")" end
      end
    else
      rec.depends[#rec.depends + 1] = id
      rec.depCons[id] = cons
    end
  end
  rec.settings = type(def.settings) == "table" and def.settings or {}
  -- "native": "libfoo.so"  or  { "Android": "libfoo.so", "Windows": "foo.dll" }: the C++ part of the mod, native/<arm64-v8a|windows-x64>/<file> in the
  -- package (see docs). It must say which platforms it is built for.
  if def.native ~= nil then
    local name = def.native
    if type(name) == "table" then name = name[kmod.platform] end
    local badName = function(n) return n == "" or n:find("[/\\]") or n:find("..", 1, true) end
    if type(def.native) == "table" and name == nil then
      if not rec.unsupported and not (rec.platforms and not rec.platforms[kmod.platform]) then rec.warnings[#rec.warnings + 1] = "native lists no library for " .. kmod.platform end
    elseif type(name) ~= "string" or badName(name) then
      rec.errors[#rec.errors + 1] = "native must be a library file name"
    else
      rec.native = name
      if def.format ~= 2 or not rec.platforms then rec.errors[#rec.errors + 1] = 'a native mod needs "format": 2 and a "platform" list' end
      local sub = kmod.native_dir or "arm64-v8a"
      if not kmod.exists(rec.dir .. "/native/" .. sub .. "/" .. name) and not rec.unsupported then
        rec.errors[#rec.errors + 1] = "native/" .. sub .. "/" .. name .. " missing"
      end
    end
  end
  if kmod.exists(rec.dir .. "/main.lua") then rec.entry = rec.dir .. "/main.lua"
  elseif kmod.exists(rec.dir .. "/Scripts/main.lua") then rec.entry = rec.dir .. "/Scripts/main.lua"; rec.warnings[#rec.warnings + 1] = "legacy Scripts/main.lua"
  elseif not rec.native and not def.paks then rec.errors[#rec.errors + 1] = "main.lua missing" end      -- a package may be paks only
  rec.enabled = state.enabled[rec.id] == true
  loadValues(rec)
  return rec
end

function kmod.scan_mods()
  local list = {}
  for _, e in ipairs(kmod.listdir(P.loaded)) do
    if e.dir then
      local rec = inspect(e.name)
      rec.status = kmod._status[rec.id]
      if rec.unsupported then rec.status = rec.unsupported
      elseif #rec.errors > 0 then rec.status = "error: " .. rec.errors[1]
      elseif not rec.enabled then
        rec.status = (kmod.auto_disabled and kmod.auto_disabled[rec.id]) or "disabled"
        if rec.native and kmod.native_loaded(rec.id) then rec.status = "disabled (restart the game to unload its native code)" end
      elseif not rec.status then rec.status = "not loaded" end
      list[#list + 1] = rec
    end
  end
  table.sort(list, function(a, b) return a.id < b.id end)
  return list
end

-- rescan the folders (UI opens, new package copied in) but keep the runtime parts of records that are already loaded
-- (handlers, values, language cache) so Settings.on_change keeps working
function kmod.refresh_mods()
  local old = {}
  for _, rec in ipairs(kmod.mods or {}) do old[rec.id] = rec end
  local list = kmod.scan_mods()
  for _, rec in ipairs(list) do
    local o = old[rec.id]
    if o then rec._handlers, rec._langHandlers, rec.values, rec._lang = o._handlers, o._langHandlers, o.values, o._lang end
  end
  kmod.mods = list
  local byId = {}
  for _, rec in ipairs(list) do byId[rec.id] = rec end
  kmod._recById = byId
  return list
end

-- ------------------------------------------------------------------ migration from the pre-v1 layout
local function migrate()
  local changed = false
  for _, e in ipairs(kmod.listdir(P.mods)) do
    if e.dir then
      local dir = P.mods .. "/" .. e.name
      local id = e.name
      local mj = json.decode(readFile(dir .. "/mod.json") or "{}")
      if type(mj) == "table" and type(mj.id) == "string" then id = mj.id end
      if kmod.exists(dir .. "/enabled.txt") then
        state.enabled[id] = true
        os.remove(dir .. "/enabled.txt")
        changed = true
        log("migrated enabled.txt of " .. id)
      end
      if kmod.exists(dir .. "/settings.json") then
        local dst = P.data .. "/" .. id .. "/settings.json"
        if not kmod.exists(dst) then
          mkdirs(P.data .. "/" .. id)
          writeFile(dst, readFile(dir .. "/settings.json") or "{}")
          os.remove(dir .. "/settings.json")
          log("migrated settings.json of " .. id)
        end
      end
    end
  end
  if changed then saveState() end

  -- the old kmod/inbox folder is gone: .kmod files left in it move to kmod/mods, and the folder is removed when it is empty
  local oldInbox = P.root .. "/inbox"
  if kmod.exists(oldInbox) then
    for _, e in ipairs(kmod.listdir(oldInbox)) do
      if not e.dir and e.name:lower():match("%.kmod$") and not kmod.exists(P.mods .. "/" .. e.name) then
        if os.rename(oldInbox .. "/" .. e.name, P.mods .. "/" .. e.name) then log("moved " .. e.name .. " from the old inbox to mods") end
      end
    end
    os.remove(oldInbox)                                           -- only succeeds when it is empty
  end
end

-- ------------------------------------------------------------------ pak files shipped inside a .kmod
-- mod.json "paks": [ {"file": "paks/…/X_P.pak", "sha256": "…", "platforms": ["Windows"]}, … ]. Every file under paks/ is declared
-- with its checksum (a pak has no version number). They are installed into the normal paks folder and are ordinary pak mods
-- from then on; kmod/pak-owners.json remembers which package put which file there. A first installation that overwrites
-- nothing happens at once. Everything else (an update, a file with the same name) waits until the player confirms, and is
-- swapped in by the core at the next game start (pak-staged/), because a mounted pak cannot be replaced.
local PAK_EXT = { pak = true, utoc = true, ucas = true, sig = true }
local GAME_PAKS = { "kards-windows", "kards-android", "pakchunk", "global." }
local OWNERS = P.root .. "/pak-owners.json"
local pakApproved = {}                                    -- package file name -> true: the player agreed to its pak changes
kmod.pak_pending = kmod.pak_pending or {}                 -- package file name -> what is waiting for the player
kmod.pak_restart = kmod.pak_restart or false              -- pak changes that need a game restart

local function loadOwners()
  local t = json.decode(readFile(OWNERS) or "{}")
  if type(t) ~= "table" then t = {} end
  if type(t.mods) ~= "table" then t.mods = {} end
  return t
end

local function saveOwners(t)
  writeFile(OWNERS .. ".tmp", json.encode(t))
  os.remove(OWNERS)
  os.rename(OWNERS .. ".tmp", OWNERS)
end

-- Windows sets the pak load order by renaming (the engine takes a pak's priority from its name): X_P.pak becomes
-- X_P.kmod_03_P.pak, sidecar files alike. pak-owners.json and mod.json keep the original name; these map between the two
-- (the launcher's pakname.rs and the core's KMod_ApplyStagedPaks do the same).
local function pakLogical(name)
  local stem, ext = name:match("^(.*)%.([^%.]+)$")
  if not stem then return name end
  repeat
    local base = stem:match("^(.+)%.kmod_%d+_P$")
    if base then stem = base end
  until not base
  return stem .. "." .. ext
end

-- names in the paks folder (without .disable) whose original name is `name`; the exact name first
local function pakOnDisk(name)
  local out, seen = {}, {}
  if kmod.exists(P.paks .. "/" .. name) or kmod.exists(P.paks .. "/" .. name .. ".disable") then out[1] = name; seen[name:lower()] = true end
  for _, e in ipairs(kmod.listdir(P.paks)) do
    local plain = e.name:gsub("%.disable$", "")
    if not e.dir and not seen[plain:lower()] and pakLogical(plain):lower() == name:lower() then
      out[#out + 1] = plain; seen[plain:lower()] = true
    end
  end
  return out
end

-- the name a file is written under in the paks folder: the name it has now, else the order tag of its .pak
local function pakTarget(name)
  for _, n in ipairs(pakOnDisk(name)) do if n ~= name then return n end end
  local stem, ext = name:match("^(.*)%.([^%.]+)$")
  if stem then
    for _, n in ipairs(pakOnDisk(stem .. ".pak")) do
      local tag = n:match("(%.kmod_%d+_P)%.[^%.]+$")
      if tag then return stem .. tag .. "." .. ext end
    end
  end
  return name
end

-- where a pak file of that name currently is: waiting in the staged folder, in the paks folder, or switched off there
local function pakPath(name)
  if kmod.exists(P.pak_staged .. "/" .. name) then return P.pak_staged .. "/" .. name end
  for _, n in ipairs(pakOnDisk(name)) do
    for _, p in ipairs({ P.paks .. "/" .. n, P.paks .. "/" .. n .. ".disable" }) do
      if kmod.exists(p) then return p end
    end
  end
  return nil
end

local function pakPlatform(v)
  if v == nil then return true end
  if type(v) == "string" then v = { v } end
  if type(v) ~= "table" then return nil end
  for _, p in ipairs(v) do if tostring(p):lower() == kmod.platform:lower() then return true end end
  return false
end

-- the package's pak files for this platform, after checking every declaration against the zip; nil + reason when they do not match
local function packagePaks(zipPath, def)
  local entries, err = kmod.zip_list(zipPath)
  if not entries then return nil, "unzip failed: " .. tostring(err) end
  local inZip = {}
  for _, e in ipairs(entries) do if e.name:lower():sub(1, 5) == "paks/" then inZip[e.name] = e.size end end
  if def.paks == nil then
    if next(inZip) then return nil, 'files in paks/ must be listed in "paks" of mod.json' end
    return {}
  end
  if type(def.paks) ~= "table" or #def.paks == 0 then return nil, '"paks" must be a non-empty list' end
  local newEnough = false                                 -- an older loader would install the Lua part and silently drop the paks
  for _, spec in ipairs(def.depends or {}) do
    local id, cons = parseDep(spec)
    if id == "kmod" and #cons > 0 and not versionOk("0.3.999", cons) then newEnough = true end
  end
  if not newEnough then return nil, 'a package with paks needs "depends": ["kmod>=0.4.0"]' end
  local declared, wanted, taken = {}, {}, {}
  for _, p in ipairs(def.paks) do
    local file = type(p) == "table" and p.file
    if type(file) ~= "string" or file:sub(1, 5) ~= "paks/" then return nil, 'every entry of "paks" needs "file": "paks/..."' end
    local name = file:match("([^/]+)$") or ""
    local ext = name:match("^[^%.].*%.(%a+)$")
    if not ext or not PAK_EXT[ext:lower()] or name:find('[\\:*?"<>|]') then return nil, "not a pak file name: " .. file end
    for _, g in ipairs(GAME_PAKS) do if name:lower():sub(1, #g) == g then return nil, "a mod may not replace the game's own " .. name end end
    local sha = type(p.sha256) == "string" and p.sha256:lower() or ""
    if #sha ~= 64 or sha:find("%X") then return nil, "sha256 missing or invalid for " .. file end
    if declared[file] then return nil, "listed twice: " .. file end
    if not inZip[file] then return nil, "declared but not in the package: " .. file end
    declared[file] = true
    local here = pakPlatform(p.platforms)
    if here == nil then return nil, "bad platforms of " .. file end
    local actual, zerr = kmod.zip_extract(zipPath, file)          -- reads and hashes the entry, writes nothing
    if not actual then return nil, tostring(zerr) end
    if actual ~= sha then return nil, "checksum mismatch: " .. file end
    if here then
      if taken[name:lower()] then return nil, "two files named " .. name .. " for " .. kmod.platform end
      taken[name:lower()] = true
      wanted[#wanted + 1] = { name = name, entry = file, sha256 = sha, size = inZip[file] }
    end
  end
  for f in pairs(inZip) do if not declared[f] then return nil, 'in the package but not listed in "paks": ' .. f end end
  for _, w in ipairs(wanted) do
    local stem = w.name:match("^(.*)%.%a+$")
    if not taken[(stem .. ".pak"):lower()] then return nil, w.name .. " has no .pak beside it" end
  end
  return wanted
end

-- what installing these files would do to the paks that are there now
local function planPaks(id, wanted)
  local owners = loadOwners()
  local rec = owners.mods[id]
  local old, names = {}, {}
  for _, f in ipairs(rec and rec.files or {}) do old[f.name] = f.sha256 end
  local plan = { write = {}, replace = {}, remove = {}, conflicts = {} }
  local same = true
  for _, w in ipairs(wanted) do names[w.name] = true; if old[w.name] ~= w.sha256 then same = false end end
  for name in pairs(old) do if not names[name] then plan.remove[#plan.remove + 1] = name; same = false end end
  table.sort(plan.remove)
  if same then plan.action = "none"; return plan end    -- already installed; a pak the player deleted by hand stays deleted
  for _, w in ipairs(wanted) do
    local cur = pakPath(w.name)
    if not cur then plan.write[#plan.write + 1] = w
    elseif old[w.name] then
      if old[w.name] ~= w.sha256 then plan.write[#plan.write + 1] = w; plan.replace[#plan.replace + 1] = w.name end
    else
      local owner
      for other, r in pairs(owners.mods) do
        if other ~= id then for _, f in ipairs(r.files or {}) do if f.name == w.name then owner = other end end end
      end
      if kmod.file_sha256(cur) ~= w.sha256 then                  -- the identical file is simply taken over
        plan.write[#plan.write + 1] = w
        plan.conflicts[#plan.conflicts + 1] = owner and (w.name .. " (" .. owner .. ")") or w.name
      end
    end
  end
  local update = rec ~= nil or kmod.exists(P.loaded .. "/" .. id)
  plan.action = (#plan.conflicts > 0 or #plan.remove > 0 or (update and #plan.write > 0)) and "confirm" or "apply"
  plan.staged = plan.action == "confirm"
  return plan
end

local function applyPaks(zipPath, def, wanted, plan)
  local dir = plan.staged and P.pak_staged or P.paks
  mkdirs(P.pak_staged); mkdirs(P.paks)
  local function undo() for _, w in ipairs(plan.write) do os.remove(dir .. "/" .. w.name .. ".tmp") end end
  for _, w in ipairs(plan.write) do                       -- everything is extracted and checked before anything is renamed
    local sha, err = kmod.zip_extract(zipPath, w.entry, dir .. "/" .. w.name .. ".tmp")
    if sha ~= w.sha256 then undo(); return false, "cannot install " .. w.name .. ": " .. tostring(err or "checksum mismatch") end
  end
  for _, w in ipairs(plan.write) do
    local dst = dir .. "/" .. (plan.staged and w.name or pakTarget(w.name))     -- staged files are placed by the core
    os.remove(P.pak_staged .. "/" .. w.name .. ".del")
    os.remove(dst)
    local ok, err = os.rename(dst .. ".tmp", dst)
    if not ok then undo(); return false, "cannot install " .. w.name .. ": " .. tostring(err) end
    log("pak " .. w.name .. (plan.staged and " staged for the next start" or " installed"))
  end
  for _, name in ipairs(plan.remove) do
    os.remove(P.pak_staged .. "/" .. name)
    writeFile(P.pak_staged .. "/" .. name .. ".del", "")
    plan.staged = true
  end
  local owners = loadOwners()
  local mine = {}
  for _, w in ipairs(wanted) do mine[w.name] = true end
  for other, r in pairs(owners.mods) do                   -- files taken over from another package are no longer its own
    if other ~= def.id then
      local keep = {}
      for _, f in ipairs(r.files or {}) do if not mine[f.name] then keep[#keep + 1] = f end end
      r.files = keep
    end
  end
  if #wanted == 0 then owners.mods[def.id] = nil
  else
    local files = {}
    for _, w in ipairs(wanted) do files[#files + 1] = { name = w.name, sha256 = w.sha256, size = w.size } end
    owners.mods[def.id] = { version = tostring(def.version), files = files }
  end
  saveOwners(owners)
  if #plan.write > 0 or #plan.remove > 0 then
    if plan.staged or kmod.platform ~= "Android" then kmod.pak_restart = true       -- Android can mount a new pak right away
    else kmod._pak_mount = true end
  end
  return true
end

-- has someone else (the launcher) installed exactly what a held-back package asked for?
local function pakAlreadyInstalled(info)
  local rec = loadOwners().mods[info.id]
  local have = {}
  for _, f in ipairs(rec and rec.files or {}) do have[#have + 1] = f.name .. ":" .. f.sha256 end
  table.sort(have)
  return table.concat(have, "|") == info.key
end

-- ------------------------------------------------------------------ .kmod installation
-- true, id  |  false, reason  |  "confirm", info (nothing installed: the player has to agree to the pak changes first)
local function installOne(zipPath, fileName)
  local tmp = P.loaded .. "/.tmp"
  kmod.rmtree(tmp); mkdirs(tmp)
  local ok, err = kmod.unzip(zipPath, tmp)
  if not ok then kmod.rmtree(tmp); return false, "unzip failed: " .. tostring(err) end
  local def, perr = json.decode(readFile(tmp .. "/mod.json") or "")
  local function fail(msg) kmod.rmtree(tmp); return false, msg end
  if type(def) ~= "table" then return fail("mod.json missing or invalid " .. tostring(perr or "")) end
  if def.format ~= 1 and def.format ~= 2 then return fail("format must be 1 or 2") end
  if type(def.id) ~= "string" or not def.id:match(ID_PAT) then return fail("invalid or missing id") end
  if def.version == nil then return fail("version is required") end
  if def.depends ~= nil and type(def.depends) ~= "table" then return fail("depends must be a list") end
  for _, spec in ipairs(def.depends or {}) do
    local id, cons = parseDep(spec)
    if not id then return fail("bad dependency " .. tostring(spec) .. " (" .. tostring(cons) .. ")") end
    local have = RESERVED[id] and reservedVersion(id)
    if have then
      local ok, bad = versionOk(have, cons)
      if not ok then return fail("requires " .. id .. bad .. " (have " .. have .. ")") end
    end
  end
  if not kmod.exists(tmp .. "/main.lua") and def.native == nil and def.paks == nil then return fail("main.lua missing") end
  local wanted, pakErr = packagePaks(zipPath, def)
  if not wanted then return fail(pakErr) end
  local plan = planPaks(def.id, wanted)
  if plan.action == "confirm" and not pakApproved[fileName] then
    kmod.rmtree(tmp)
    local key, write = {}, {}
    for _, w in ipairs(wanted) do key[#key + 1] = w.name .. ":" .. w.sha256 end
    for _, w in ipairs(plan.write) do write[#write + 1] = w.name end
    table.sort(key)
    return "confirm", { id = def.id, name = tostring(def.name or def.id), version = tostring(def.version), key = table.concat(key, "|"),
                        write = write, remove = plan.remove, conflicts = plan.conflicts }
  end
  if plan.action ~= "none" then
    local ok, aerr = applyPaks(zipPath, def, wanted, plan)
    if not ok then return fail(aerr) end
  end
  pakApproved[fileName] = nil
  local target = P.loaded .. "/" .. def.id
  if kmod.exists(target) then kmod.rmtree(target) end   -- upgrade = replace the package; data/ and state.json are untouched
  if not os.rename(tmp, target) then return fail("could not move package into mods/") end
  return true, def.id
end

-- Mirrors kmod/mods (shared storage: mod folders and .kmod packages) into the private folder the mods are loaded from.
-- Folders are copied when anything in them changed, .kmod files are extracted when the file changed; private copies whose source
-- is gone are removed. A folder and a .kmod with the same id: the folder wins. Returns how many mods were added or updated.
kmod.sync_errors = kmod.sync_errors or {}
function kmod.sync_mods()
  mkdirs(P.mods); mkdirs(P.loaded)
  local stamps = P.loaded .. "/.stamps"
  mkdirs(stamps)
  local changed, present, keepStamp = 0, {}, {}
  kmod.pak_pending = {}
  local entries = kmod.listdir(P.mods)
  for _, e in ipairs(entries) do                                  -- 1. mod folders
    if e.dir then
      local src, dst = P.mods .. "/" .. e.name, P.loaded .. "/" .. e.name
      local sf = "dir_" .. e.name .. ".txt"
      keepStamp[sf] = true; present[e.name] = true
      local st = kmod.tree_stamp(src)
      if readFile(stamps .. "/" .. sf) ~= st or not kmod.exists(dst) then
        kmod.rmtree(dst)
        local ok, err = kmod.copytree(src, dst)
        if ok then writeFile(stamps .. "/" .. sf, st); changed = changed + 1; log("synced " .. e.name)
        else logw("could not copy " .. e.name .. ": " .. tostring(err)); kmod.rmtree(dst); present[e.name] = nil end
      end
    end
  end
  for _, e in ipairs(entries) do                                  -- 2. .kmod packages
    if not e.dir and e.name:lower():match("%.kmod$") then
      local src = P.mods .. "/" .. e.name
      local sf = "kmod_" .. e.name .. ".txt"
      keepStamp[sf] = true
      local st = kmod.tree_stamp(src)
      local saved = readFile(stamps .. "/" .. sf) or ""
      local savedStamp, savedId = saved:match("^([^\n]*)\n?(.*)$")
      local held = savedStamp == st and savedId:sub(1, 8) == "confirm:" and json.decode(savedId:sub(9))
      if type(held) == "table" and not pakApproved[e.name] and not pakAlreadyInstalled(held) then
        kmod.pak_pending[e.name] = held; present[held.id] = true      -- the installed version stays until the player agrees
      elseif savedStamp == st and savedId ~= "" and savedId:sub(1, 8) ~= "confirm:" and (savedId:sub(1, 7) == "failed:" or kmod.exists(P.loaded .. "/" .. savedId)) then
        if savedId:sub(1, 7) == "failed:" then kmod.sync_errors[e.name] = savedId:sub(8)
        elseif present[savedId] then logw(e.name .. ": a folder with the same id exists; the folder is used")
        else present[savedId] = true end
      else
        local ok, res = installOne(src, e.name)
        if ok == "confirm" then
          writeFile(stamps .. "/" .. sf, st .. "\nconfirm:" .. json.encode(res)); present[res.id] = true
          kmod.pak_pending[e.name] = res; kmod.sync_errors[e.name] = nil
          logw(e.name .. ": it changes pak files; confirm on the Mods page (console: paks confirm " .. e.name .. ")")
        elseif ok then
          if present[res] and kmod.exists(P.mods .. "/" .. res) then logw(e.name .. ": a folder with the same id (" .. res .. ") exists; the folder is used")
          else writeFile(stamps .. "/" .. sf, st .. "\n" .. res); present[res] = true; changed = changed + 1; kmod.sync_errors[e.name] = nil; log("extracted " .. res .. " from " .. e.name) end
        else
          writeFile(stamps .. "/" .. sf, st .. "\nfailed:" .. tostring(res)); kmod.sync_errors[e.name] = tostring(res)
          logw("cannot install " .. e.name .. ": " .. tostring(res))
        end
      end
    end
  end
  for _, e in ipairs(kmod.listdir(P.loaded)) do                   -- 3. private copies whose source is gone
    if e.dir and not present[e.name] then kmod.rmtree(P.loaded .. "/" .. e.name); log("removed " .. e.name .. " (no longer in kmod/mods)") end
  end
  for _, e in ipairs(kmod.listdir(stamps)) do
    if not keepStamp[e.name] then os.remove(stamps .. "/" .. e.name) end
  end
  if kmod._pak_mount then kmod._pak_mount = false; if kmod.mount_pending_paks then kmod.mount_pending_paks() end end
  return changed
end

-- The player agreed to the pak changes of a held-back package. Returns true, or false + the reason.
function kmod.confirm_paks(file)
  if not kmod.pak_pending[file] then return false, "nothing to confirm for " .. tostring(file) end
  pakApproved[file] = true
  kmod.sync_mods()
  pakApproved[file] = nil
  if kmod.sync_errors[file] then return false, kmod.sync_errors[file] end
  return true
end

local lastLang = nil

-- ------------------------------------------------------------------ running mods
local function makeRequire(rec, env)
  local loaded = {}
  return function(name)
    if loaded[name] ~= nil then return loaded[name] end
    local rel = name:gsub("%.", "/")
    local path = rec.dir .. "/" .. rel .. ".lua"
    if not kmod.exists(path) then path = rec.dir .. "/" .. rel .. "/init.lua" end
    local chunk, err = loadfile(path, "t", env)
    if not chunk then error("module '" .. name .. "' not found in mod " .. rec.id .. ": " .. tostring(err), 2) end
    local r = chunk(name)
    if r == nil then r = true end
    loaded[name] = r
    return r
  end
end

local function loadOne(rec)
  rec._handlers, rec._langHandlers = {}, {}
  local env = setmetatable({}, { __index = _G })
  for k, v in pairs(kmod.mod_api(rec.id)) do env[k] = v end          -- owner-tagged RegisterHook/ExecuteWithDelay/...
  env.print = function(...) kmod.log("info", rec.id, ...) end
  env.Log = {
    debug = function(...) kmod.log("debug", rec.id, ...) end,
    info = function(...) kmod.log("info", rec.id, ...) end,
    warn = function(...) kmod.log("warn", rec.id, ...) end,
    error = function(...) kmod.log("error", rec.id, ...) end,
  }
  env.Worker = function(file)   -- Worker("worker.lua"): a thread with its own Lua state; script path relative to the mod folder
    assert(type(file) == "string" and not file:find("..", 1, true) and not file:find("^[/\\]"), "Worker: the script must be a relative path inside the mod")
    return kmod.worker(rec.id, rec.dir .. "/" .. file)
  end
  if kmod.net_sockets_api then env.WebSocket, env.Socket = kmod.net_sockets_api(rec) end
  if kmod.net_api then env.Net = kmod.net_api(rec) end          -- the game's own WebSocket traffic (permission "game_network")
  if kmod.ui_api then env.UI = kmod.ui_api(rec) end            -- UI.window{...}, UI.toast(text)
  -- Media.pick_image(dest, cb): the player picks an image (Android: the gallery / system chooser; Windows: a file dialog). It is saved to
  -- dest + ".png" (Windows keeps the picked file's own extension: .png .jpg .bmp) and cb(path) runs on the game thread. cb(nil, "cancelled")
  -- if the player chose nothing, cb(nil, message) on errors. dest (no extension) must lie inside kmod/art or this mod's own kmod/data folder.
  -- Returns false if the platform cannot show a chooser. Not available in the dev console of an old loader: check `Media ~= nil`.
  -- Input (Windows; on Android there is no keyboard and nothing ever fires): Input.is_down("F8" | "CTRL+SHIFT+K") and Input.on_key(key, fn): fn() runs each
  -- time the key combination goes down (checked every 40 ms, only while the game window has the focus). Keys: A-Z, 0-9, F1-F24, CTRL, SHIFT, ALT, ESC, ENTER,
  -- SPACE, TAB, INSERT, DELETE, HOME, END, PAGEUP, PAGEDOWN, UP, DOWN, LEFT, RIGHT, TILDE.
  env.Input = {
    is_down = function(k) return kmod.key_down(k) end,
    on_key = function(k, fn)
      assert(type(k) == "string" and type(fn) == "function", "Input.on_key(key, callback)")
      -- debounce: the combination must have been up for 3 checks (120 ms) before it can fire again (the focus can flicker while a window opens)
      local armed, upTicks = true, 3
      env.LoopAsync(40, function()
        if kmod.key_down(k) then
          if armed then armed = false; fn() end
          upTicks = 0
        else
          upTicks = upTicks + 1
          if upTicks >= 3 then armed = true end
        end
        return false
      end)
    end,
  }
  env.Media = {
    pick_image = function(dest, cb)
      assert(type(dest) == "string" and type(cb) == "function", "Media.pick_image(dest, callback)")
      local d = dest:gsub("\\", "/")
      local okPath = not d:find("..", 1, true)
        and (d:find(P.root .. "/art/", 1, true) == 1 or d:find(P.data .. "/" .. rec.id .. "/", 1, true) == 1)
      if not okPath then error("Media.pick_image: the destination must be inside kmod/art or this mod's data folder", 2) end
      return kmod.pick_image(rec.id, d, cb)
    end,
    -- Media.pick_file(dest, exts, cb): like pick_image, but any file whose extension is in exts ("artpack,zip"); copied as is to dest + "." + ext.
    -- cb(path, original_name). Media.save_file(src, suggested_name, cb): "save as": the player chooses where src goes; cb(chosen_name). src must lie
    -- inside kmod/art or this mod's data folder.
    pick_file = function(dest, exts, cb)
      assert(type(dest) == "string" and type(exts) == "string" and type(cb) == "function", "Media.pick_file(dest, exts, callback)")
      local d = dest:gsub("\\", "/")
      if d:find("..", 1, true) or not (d:find(P.root .. "/art/", 1, true) == 1 or d:find(P.data .. "/" .. rec.id .. "/", 1, true) == 1) then
        error("Media.pick_file: the destination must be inside kmod/art or this mod's data folder", 2)
      end
      return kmod.pick_file(rec.id, d, exts, cb)
    end,
    save_file = function(src, name, cb)
      assert(type(src) == "string" and type(name) == "string" and type(cb) == "function", "Media.save_file(src, name, callback)")
      local s = src:gsub("\\", "/")
      if s:find("..", 1, true) or not (s:find(P.root .. "/art/", 1, true) == 1 or s:find(P.data .. "/" .. rec.id .. "/", 1, true) == 1) then
        error("Media.save_file: the source must be inside kmod/art or this mod's data folder", 2)
      end
      return kmod.save_file(rec.id, s, (name:gsub("[\\/:*?\"<>|]", "_")), cb)
    end,
    zip = function(srcDir, destZip)
      local s, d = srcDir:gsub("\\", "/"), destZip:gsub("\\", "/")
      for _, x in ipairs({ s, d }) do
        if x:find("..", 1, true) or not (x:find(P.root .. "/art/", 1, true) == 1 or x:find(P.data .. "/" .. rec.id .. "/", 1, true) == 1) then
          error("Media.zip: both paths must be inside kmod/art or this mod's data folder", 2)
        end
      end
      return kmod.zip(s, d)
    end,
  }
  -- Http: needs "permissions": ["network"] in mod.json. Http.get(url [, opts], cb), Http.post(url, body [, opts], cb), Http.request(opts, cb), Http.json(resp)
  local function httpRequest(opts, cb)
    if not rec.permissions.network then error('Http: this mod must declare "permissions": ["network"] in mod.json', 3) end
    if type(opts) ~= "table" or type(cb) ~= "function" then error("Http: expected (options, callback)", 3) end
    return kmod.http_request(rec.id, opts, cb)
  end
  local function withUrl(url, method, opts)
    local o = {}
    for k, v in pairs(opts or {}) do o[k] = v end
    o.url, o.method = url, method
    return o
  end
  env.Http = {
    request = httpRequest,
    get = function(url, opts, cb)
      if type(opts) == "function" then cb, opts = opts, nil end
      return httpRequest(withUrl(url, "GET", opts), cb)
    end,
    post = function(url, body, opts, cb)
      if type(opts) == "function" then cb, opts = opts, nil end
      local o = withUrl(url, "POST", opts)
      if type(body) == "table" then
        o.body = json.encode(body)
        o.headers = o.headers or {}
        if not o.headers["Content-Type"] then o.headers["Content-Type"] = "application/json" end
      else o.body = body end
      return httpRequest(o, cb)
    end,
    -- Http.download(url, path [, opts], cb [, progress]): streams the body into a file inside kmod/data, mods, paks or art.
    -- cb(resp) with resp.saved / resp.size on success; progress(bytes, total) about 6 times a second (total = -1 if unknown)
    download = function(url, path, opts, cb, progress)
      if type(opts) == "function" then progress, cb, opts = cb, opts, nil end
      local o = withUrl(url, "GET", opts)
      o.save_to = path
      if not rec.permissions.network then error('Http: this mod must declare "permissions": ["network"] in mod.json', 2) end
      if type(cb) ~= "function" then error("Http.download: expected a callback", 2) end
      return kmod.http_request(rec.id, o, cb, progress)
    end,
    json = function(resp) return resp and resp.body and json.decode(resp.body) or nil end,
  }
  -- Events: a bus shared by all mods (cleared when mods reload). Events.on(name, fn) -> handle; Events.emit(name, ...); Events.off(handle)
  kmod._events = kmod._events or {}
  env.Events = {
    on = function(name, fn)
      assert(type(name) == "string" and type(fn) == "function", "Events.on(name, fn)")
      local list = kmod._events[name] or {}
      kmod._events[name] = list
      local h = { name = name, fn = fn, owner = rec.id }
      list[#list + 1] = h
      return h
    end,
    off = function(h)
      local list = type(h) == "table" and kmod._events[h.name]
      if list then for i, x in ipairs(list) do if x == h then table.remove(list, i); break end end end
    end,
    emit = function(name, ...)
      local list = kmod._events[name]
      if not list then return 0 end
      local copy, n = { table.unpack(list) }, 0
      for _, h in ipairs(copy) do
        local ok, err = pcall(h.fn, ...)
        n = n + 1
        if not ok then loge(h.owner .. ": Events handler '" .. name .. "' failed: " .. tostring(err)); if kmod._mod_error then kmod._mod_error(h.owner, tostring(err)) end end
      end
      return n
    end,
  }
  -- Store: small persistent key/value storage in kmod/data/<id>/store.json (values: anything json can hold)
  local storePath = P.data .. "/" .. rec.id .. "/store.json"
  local store
  local function loadStore() if not store then store = json.decode(readFile(storePath) or "{}") or {}; end return store end
  env.Store = {
    get = function(key, default) local v = loadStore()[key]; if v == nil then return default end return v end,
    set = function(key, value) loadStore()[key] = value; mkdirs(P.data .. "/" .. rec.id); writeFile(storePath, json.encode(store)) end,
  }
  env.Mod = { data_dir = P.data .. "/" .. rec.id, id = rec.id, name = rec.name, version = rec.version, dir = rec.dir, path = function(rel) return rec.dir .. "/" .. rel end }
  env.Settings = {
    get = function(id) return kmod.settings_get(rec, id) end,
    set = function(id, v) kmod.settings_set(rec, id, v) end,
    on_change = function(fn) rec._handlers[#rec._handlers + 1] = fn end,
  }
  env.Lang = {
    t = function(key, vars)
      local s = kmod.rec_text(rec, key)
      return s and interpolate(s, vars) or key
    end,
    current = kmod.lang_current,
    on_change = function(fn) rec._langHandlers[#rec._langHandlers + 1] = fn end,
  }
  if kmod.console_api then env.Console = kmod.console_api(rec) end
  env.require = makeRequire(rec, env)
  if kmod.ue4ss_api then kmod.ue4ss_api(rec, env) end                -- delayed actions, lifecycle hooks, mod management (ue4ss.lua)
  if rec.native then                                                    -- C++ part first: it may add functions to `Native`
    local ok, nerr = kmod.native_load(rec.id, rec.dir, rec.native)
    if not ok then rec.status = "native error: " .. tostring(nerr); kmod._status[rec.id] = rec.status; loge(rec.id .. ": " .. rec.status); return end
    env.Native = (kmod._natives and kmod._natives[rec.id]) or {}
    if not rec.entry then rec.status = "loaded"; kmod._status[rec.id] = rec.status; log("loaded " .. rec.id .. " v" .. rec.version .. " (native)"); return end
  end
  if not rec.entry then rec.status = "loaded"; kmod._status[rec.id] = rec.status; log("loaded " .. rec.id .. " v" .. rec.version .. " (paks only)"); return end
  local chunk, err = loadfile(rec.entry, "t", env)
  if not chunk then rec.status = "load error: " .. tostring(err); kmod._status[rec.id] = rec.status; loge(rec.id .. ": " .. rec.status); return end
  kmod.set_activity("mod", rec.id, "load")        -- a crash while the mod's main.lua runs is blamed on it
  local ok, e = xpcall(chunk, debug.traceback)
  kmod.set_activity()
  if ok then rec.status = "loaded" else rec.status = "error: " .. tostring(e):match("^[^\n]*") ; loge(rec.id .. " main.lua failed: " .. tostring(e)) end
  kmod._status[rec.id] = rec.status
  if ok then log("loaded " .. rec.id .. " v" .. rec.version) end
end

-- crash recovery state (see kmod_boot.h): safe mode, mods/paks blamed for a crash
kmod.boot = kmod.boot_info()
function kmod.refresh_boot() kmod.boot = kmod.boot_info(); return kmod.boot end

function kmod.load_mods()
  kmod.refresh_boot()
  if kmod.boot.safe then logw("SAFE MODE: no mods are loaded; use the 'safe off' console command or the Mods page to leave it") end
  loadState()
  mkdirs(P.data); mkdirs(P.mods)
  migrate()
  kmod.sync_mods()
  kmod._status = {}
  kmod._events = {}
  kmod._net_conns = {}
  if kmod.ui_close_all then kmod.ui_close_all() end        -- windows of the previous run
  if kmod.console_clear_mods then kmod.console_clear_mods() end     -- mod commands are re-registered by the mods that load below
  if not kmod.game then
    if kmod.detect_game_version() then log("game version " .. tostring(kmod.game_version) .. " (kards " .. kmod.game .. ")")
    else logw("game version unknown; kards>= conditions are ignored") end
  end
  local mods = kmod.scan_mods()
  kmod.mods = mods
  local byId = {}
  for _, rec in ipairs(mods) do byId[rec.id] = rec end
  kmod._recById = byId

  local order, mark = {}, {}
  local function visit(rec)
    if mark[rec.id] == "done" then return rec.ok end
    if mark[rec.id] == "visiting" then rec.status = "dependency cycle"; return false end
    mark[rec.id] = "visiting"
    for _, dep in ipairs(rec.depends) do
      local d = byId[dep]
      if not d or #d.errors > 0 or not d.enabled then
        rec.status = "missing dependency: " .. tostring(dep); mark[rec.id] = "done"; rec.ok = false; kmod._status[rec.id] = rec.status; return false
      end
      local vok, bad = versionOk(d.version, rec.depCons[dep] or {})
      if not vok then
        rec.status = "dependency version: " .. dep .. bad .. " required (have " .. (d.version ~= "" and d.version or "?") .. ")"
        mark[rec.id] = "done"; rec.ok = false; kmod._status[rec.id] = rec.status; return false
      end
      if not visit(d) then
        rec.status = "dependency failed: " .. tostring(dep); mark[rec.id] = "done"; rec.ok = false; kmod._status[rec.id] = rec.status; return false
      end
    end
    mark[rec.id] = "done"; rec.ok = true
    order[#order + 1] = rec
    return true
  end
  for _, rec in ipairs(mods) do
    for _, w in ipairs(rec.warnings) do logw(rec.id .. ": " .. w) end
    for _, e in ipairs(rec.errors) do logw(rec.id .. ": " .. e) end
    if rec.enabled and #rec.errors == 0 then
      if kmod.boot.layout_mismatch then
        rec.status = "not loaded: this game build's engine layout differs from the one kmod was built for"; kmod._status[rec.id] = rec.status
      elseif kmod.boot.safe then
        rec.status = "not loaded: safe mode"; kmod._status[rec.id] = rec.status
      elseif kmod.boot.mods[rec.id] then
        rec.status = "not loaded: it crashed the game (enable it again to retry)"; kmod._status[rec.id] = rec.status
        logw(rec.id .. ": " .. rec.status)
      elseif not visit(rec) and rec.status then logw(rec.id .. ": " .. rec.status) end
    end
  end
  do                                                                   -- private copies of native libraries: keep only those of enabled mods
    local keepIds = {}
    for _, rec in ipairs(order) do if rec.native then keepIds[#keepIds + 1] = rec.id end end
    kmod.native_prune(keepIds)
  end
  for _, rec in ipairs(order) do loadOne(rec) end
  log(string.format("%d package(s), %d loaded", #mods, #order))
  lastLang = kmod.lang_current()
end

-- Reload ONE Lua mod: forget what it registered (hooks, timers, workers, windows, events, commands), read its folder again and run its main.lua.
-- Native mods cannot be reloaded one by one (their code stays loaded until the game restarts). Returns true, or false + the reason.
function kmod.reload_mod(id)
  local old = kmod._recById and kmod._recById[id]
  if not old then return false, "unknown mod" end
  if old.native then return false, "native mods need a restart" end
  kmod.sync_mods()
  kmod.release_owner(id)
  if kmod.ui_close_all then kmod.ui_close_all(id) end
  for _, list in pairs(kmod._events or {}) do
    for i = #list, 1, -1 do if list[i].owner == id then table.remove(list, i) end end
  end
  if kmod.console_clear_mods then kmod.console_clear_mods(id) end
  kmod._errCount[id] = nil
  kmod._status[id] = nil
  local fresh
  for _, r in ipairs(kmod.scan_mods()) do if r.id == id then fresh = r end end
  if not fresh then return false, "the mod folder is gone" end
  fresh.ok = true
  for i, r in ipairs(kmod.mods) do if r.id == id then kmod.mods[i] = fresh end end
  kmod._recById[id] = fresh
  log("reloading " .. id)
  if fresh.enabled and #fresh.errors == 0 and not fresh.native then loadOne(fresh) end
  return true
end

-- ------------------------------------------------------------------ auto-disable on repeated runtime errors
-- Called by the core for every error raised inside a mod's hook / timer / new-object callback.
-- A mod that raises MAX_ERRORS errors in one session is disabled (persisted) and the mods are reloaded.
local MAX_ERRORS = 10
kmod.max_errors = MAX_ERRORS
kmod._errCount = kmod._errCount or {}
kmod.auto_disabled = kmod.auto_disabled or {}      -- id -> reason, this session
function kmod._mod_error(id, msg)
  if kmod.auto_disabled[id] then return end
  local n = (kmod._errCount[id] or 0) + 1
  kmod._errCount[id] = n
  if n < MAX_ERRORS then return end
  local reason = "disabled after " .. n .. " script errors: " .. tostring(msg):match("^[^\n]*")
  kmod.auto_disabled[id] = reason
  loge(id .. ": " .. reason)
  kmod.set_enabled(id, false)
  ExecuteWithDelay(100, function() kmod.reload_mods() end)   -- not inside the failing callback
end

-- ------------------------------------------------------------------ language change notifications
function kmod._notify_language()
  local cur = kmod.lang_current()
  if cur == lastLang then return end
  lastLang = cur
  for _, rec in ipairs(kmod.mods or {}) do
    for _, fn in ipairs(rec._langHandlers or {}) do
      local ok, err = pcall(fn, cur)
      if not ok then loge(rec.id .. " Lang.on_change error: " .. tostring(err)) end
    end
  end
  if kmod.on_language_changed then pcall(kmod.on_language_changed, cur) end
end

if not kmod._lang_watch then                              -- poll the game language (cheap) so Lang.on_change also fires for "auto"
  kmod._lang_watch = true
  LoopAsync(2000, function() kmod._notify_language(); return false end)
end

-- ------------------------------------------------------------------ pak mods
-- Every *.pak in kmod/paks is mounted by the core (core.cpp) as soon as the engine is ready, with a pak order above the
-- game's own paks. Crashes while mounting quarantine the pak; kmod.pak_off(name, true) switches one off for the next start.
-- kmod.mount_paks() mounts paks that were dropped in later or skipped by safe mode.
function kmod.mount_paks() kmod.mount_pending_paks() end
