-- kmod built-in mod manager UI (runs as a built-in script, not a user mod). Data comes from modloader.lua.
-- Adds a "Mods" tab to the game's Options screen. The page imitates the game's own settings page: a language row, then one
-- section per mod, top to bottom: mod name (section header), version, [status], enabled checkbox, the settings the mod declares.
-- Re-running this file (dofile) is safe: it tears the old UI down first.
--
-- NEVER instantiate the game's own pages/sections (W_OptionsSettings_C, W_OptionsSettingsOther_C, ...): their entries'
-- NamedSlot contents are bound to the ORIGINAL widget objects, so a new instance steals the controls from the real settings
-- page. We only READ the game's styles and build our own plain widgets.

local function log(...) kmod.log("info", "manager", ...) end

local PATH = {
  tab   = "WBP_NUI_MasterTextButton_C:BndEvt__Button_0_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature",
  check = "WBP_NUI_MasterCheckbox_C:BndEvt__WBP_NUI_Checkbox_NUI_CheckBox_K2Node_ComponentBoundEvent_0_OnCheckBoxComponentStateChanged__DelegateSignature",
  slide = "WBP_NUI_MasterSlider_C:BndEvt__WBP_NUI_MasterSlider_Slider_K2Node_ComponentBoundEvent_3_OnFloatValueChangedEvent__DelegateSignature",
  text  = "W_EditableTextBox_C:BndEvt__W_EditableTextBox_EditableTextBox_K2Node_ComponentBoundEvent_1_OnEditableTextBoxCommittedEvent__DelegateSignature",
  drop  = "WBP_NUI_MasterDropdown_C:BndEvt__WBP_NUI_MasterDropdown_MasterDropdown_ComboBox_K2Node_ComponentBoundEvent_4_OnSelectionChanged__DelegateSignature",
}
local lib, gp, scrollCls, scaleCls, vboxCls, sizeCls, textCls, borderCls, canvasCls, richCls
local classCache = {}
local ui = nil          -- the injected UI of the live Options screen
local H = {}            -- event handlers (replaced on every run so hooks are registered only once)
kmod._mmh = H

local function wclass(name)
  local c = classCache[name]
  if not c or not c:IsValid() then c = FindObject("WidgetBlueprintGeneratedClass", name); classCache[name] = c end
  return c
end

local function statics()
  lib = lib or StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary")
  gp = gp or StaticFindObject("/Script/Engine.Default__GameplayStatics")
  scrollCls = scrollCls or StaticFindObject("/Script/UMG.ScrollBox")
  scaleCls = scaleCls or StaticFindObject("/Script/UMG.ScaleBox")
  vboxCls = vboxCls or StaticFindObject("/Script/UMG.VerticalBox")
  sizeCls = sizeCls or StaticFindObject("/Script/UMG.SizeBox")
  textCls = textCls or StaticFindObject("/Script/UMG.TextBlock")
  borderCls = borderCls or StaticFindObject("/Script/UMG.Border")
  canvasCls = canvasCls or StaticFindObject("/Script/UMG.CanvasPanel")
  richCls = richCls or StaticFindObject("/Script/UMG.RichTextBlock")
  return lib:IsValid() and gp:IsValid() and scrollCls:IsValid() and scaleCls:IsValid() and vboxCls:IsValid()
         and sizeCls:IsValid() and textCls:IsValid() and borderCls:IsValid() and canvasCls:IsValid() and richCls:IsValid()
end

-- find a widget by name below `root` (descends into UserWidget trees, panels and content widgets)
local function findWidget(root, name, depth)
  depth = depth or 0
  if depth > 28 or not root or not root:IsValid() then return nil end
  if root:GetName() == name then return root end
  local okt, tree = pcall(function() return root.WidgetTree end)
  if okt and tree and tree:IsValid() then
    local r = tree.RootWidget
    if r:IsValid() then local f = findWidget(r, name, depth + 1); if f then return f end end
  end
  local okc, n = pcall(function() return root:GetChildrenCount() end)
  if okc and n then
    for i = 0, n - 1 do local f = findWidget(root:GetChildAt(i), name, depth + 1); if f then return f end end
  else
    local okx, content = pcall(function() return root:GetContent() end)
    if okx and content and content:IsValid() then return findWidget(content, name, depth + 1) end
  end
  return nil
end

local function create(className) return lib:Create(ui.layout, wclass(className), FindFirstOf("PlayerController")) end

-- read-only snapshot of the game's styles (page title, section header, lines, scale box, canvas slot)
local function captureStyle()
  local page = ui.cs:GetChildAt(0)                         -- the game's own Settings page (read only)
  local sb = findWidget(page, "SettingsScrollBox")
  local scale = findWidget(page, "ScaleBox")
  local gr = findWidget(page, "WBP_NUI_Graphics")
  if not (sb and sb:IsValid() and scale and scale:IsValid() and gr and gr:IsValid()) then return false end
  local hdr, line = sb:GetChildAt(0), sb:GetChildAt(1)
  local groot = gr.WidgetTree.RootWidget
  local sh, sl = groot:GetChildAt(0), groot:GetChildAt(1)
  local cslot = scale.Slot                                   -- the game puts its ScaleBox in a CanvasPanel slot
  ui.style = {
    scaleBox = scale,
    layout = cslot.LayoutData, autoSize = cslot.bAutoSize,
    title = { font = hdr.Font, color = hdr.ColorAndOpacity },
    titleLine = { brush = line.Background, color = line.BrushColor, pad = line.Padding },
    sec = { font = sh.Font, color = sh.ColorAndOpacity },
    secLine = { brush = sl.Background, color = sl.BrushColor, pad = sl.Padding },
  }
  return true
end

local function header(text, kind)                          -- kind: "title" (big, orange line) | "sec" (smaller, white line)
  local st = ui.style[kind]
  local ln = ui.style[kind == "title" and "titleLine" or "secLine"]
  local tb = gp:SpawnObject(textCls, ui.layout.WidgetTree)
  local tslot = ui.scroll:AddChild(tb)
  tslot:SetPadding({ Left = 0, Top = (kind == "sec") and 36 or 0, Right = 0, Bottom = 0 })
  tb:SetText(text); tb:SetFont(st.font); tb:SetColorAndOpacity(st.color)
  local b = gp:SpawnObject(borderCls, ui.layout.WidgetTree)
  local bslot = ui.scroll:AddChild(b)
  bslot:SetPadding({ Left = 0, Top = 0, Right = 0, Bottom = 8 })
  b:SetBrush(ln.brush); b:SetBrushColor(ln.color); b:SetPadding(ln.pad)
end

local function section(title) header(title, "sec"); return ui.scroll end

-- A mod's description: its own full-width, wrapping text block in the same font as the settings labels. (As the label of a
-- settings row it was squeezed into the narrow label column and shrunk.)
local function description(text)
  if not ui.style.descFont then
    local ok, f = pcall(function() return create("W_OptionsEntryBase_C").OptionNameText.Font end)   -- the game's own label font
    ui.style.descFont = (ok and f) or ui.style.sec.font
  end
  local tb = gp:SpawnObject(textCls, ui.layout.WidgetTree)
  local tslot = ui.scroll:AddChild(tb)
  tslot:SetPadding({ Left = 0, Top = 0, Right = 0, Bottom = 12 })
  tb:SetFont(ui.style.descFont); tb:SetColorAndOpacity(ui.style.sec.color)
  pcall(function() tb:SetAutoWrapText(true) end)
  tb:SetText(text)
end

-- a settings row: label on the left, optional control in the content slot (the game's W_OptionsEntryBase_C, standalone = safe)
local function row(box, label, control)
  local r = create("W_OptionsEntryBase_C")
  box:AddChild(r)
  r.OptionNameText:SetText(label)
  pcall(function() r.HelpButtonSizeBox:SetVisibility(1) end)        -- collapse the help "?" button
  if control then r.ContentSlot:SetContent(control) end
  return r
end

local function track(w, info) ui.ctrls[w:GetAddress()] = info; return w end
local function fmtNumber(v) return (string.format("%.3f", v):gsub("0+$", ""):gsub("%.$", "")) end

local function settingRow(box, rec, s)
  local cur = kmod.settings_get(rec, s.id)
  local label = kmod.setting_label(rec, s)
  if s.type == "bool" then
    local c = create("WBP_NUI_MasterCheckbox_C")
    row(box, label, c)
    ui.updating = true; c:SetIsChecked(cur and true or false); ui.updating = false
    track(c, { kind = "bool", rec = rec, id = s.id })
  elseif s.type == "number" then
    local c = create("WBP_NUI_MasterSlider_C")
    local r = row(box, label .. " (" .. fmtNumber(cur or 0) .. ")", c)
    local sl = c.Slider
    ui.updating = true
    sl:SetMinValue(s.min or 0); sl:SetMaxValue(s.max or 1); sl:SetStepSize(s.step or 0.01)
    c.MinValue = s.min or 0; c.MaxValue = s.max or 1; c.StepSize = s.step or 0.01; c.DefaultValue = s.default or 0
    sl:SetValue(cur or 0)
    ui.updating = false
    track(c, { kind = "number", rec = rec, id = s.id, row = r, label = label })
  elseif s.type == "choice" then
    local c = create("WBP_NUI_MasterDropdown_C")
    row(box, label, c)
    ui.updating = true
    c:ClearOptions()
    local sel = 0
    for i, o in ipairs(s.options or {}) do c:AddOption(kmod.option_label(rec, s, o)); if o == cur then sel = i - 1 end end
    c:SetSelectedIndex(sel)
    ui.updating = false
    track(c, { kind = "choice", rec = rec, id = s.id, options = s.options or {} })
  elseif s.type == "string" then
    local c = create("W_EditableTextBox_C")
    local r = row(box, label, c)
    local tb = c.EditableTextBox
    pcall(function() tb:SetText(cur or "") end)
    track(c, { kind = "string", rec = rec, id = s.id, maxlen = s.maxlen })
  else
    row(box, label .. " (unsupported type '" .. tostring(s.type) .. "')")
  end
end

local function languageRow(box)
  local c = create("WBP_NUI_MasterDropdown_C")
  row(box, kmod.t("language"), c)
  ui.updating = true
  c:ClearOptions()
  local values = { "auto" }
  for _, tag in ipairs(kmod.language_tags) do values[#values + 1] = tag end
  local sel = 0
  for i, v in ipairs(values) do
    c:AddOption(v == "auto" and kmod.t("auto") or kmod.language_names[v])
    if v == kmod.state.language then sel = i - 1 end
  end
  c:SetSelectedIndex(sel)
  ui.updating = false
  track(c, { kind = "lang", options = values })
end

local function refresh()
  local t0 = os.clock()
  local s0, sm0, p0, pm0 = kmod.perf()
  kmod.perf_sample(true)
  if not ui.style then captureStyle() end
  pcall(function() ui.scaleBox:SetUserSpecifiedScale(ui.style.scaleBox.UserSpecifiedScale) end)   -- follow the UI-scale option
  local t1 = os.clock()
  if kmod.sync_mods() > 0 then kmod.reload_mods() end          -- mods dropped into kmod/mods
  local t2 = os.clock()
  ui.tab:SetText(kmod.t("mods"))
  ui.scroll:ClearChildren()
  header(kmod.t("mods"), "title")
  ui.ctrls = {}
  local mods = kmod.refresh_mods()
  languageRow(section(kmod.t("language")))
  if #mods == 0 then section(kmod.t("no_mods")); description(kmod.t("no_mods_hint")) end      -- same text block as a mod's description (a row label is squeezed small)
  local pending = {}                                           -- SetText on text blocks must run after they are parented
  for _, rec in ipairs(mods) do
    local box = section(kmod.mod_title(rec))
    local desc = kmod.mod_description(rec)
    if desc ~= "" then description(desc) end
    local vt = create("WBP_NUI_MasterTextBlock_C")
    row(box, kmod.t("version"), vt)
    pending[#pending + 1] = function() vt:SetText(rec.version ~= "" and rec.version or "-") end
    if #rec.errors > 0 or (rec.enabled and rec.status ~= "loaded") then
      local st = create("WBP_NUI_MasterTextBlock_C")
      row(box, kmod.t("status"), st)
      pending[#pending + 1] = function() st:SetText(rec.status or "") end
    end
    if #rec.errors == 0 then
      local c = create("WBP_NUI_MasterCheckbox_C")
      row(box, kmod.t("enabled"), c)
      ui.updating = true; c:SetIsChecked(rec.enabled); ui.updating = false
      track(c, { kind = "enable", rec = rec })
      for _, s in ipairs(rec.settings) do
        local ok, err = pcall(settingRow, box, rec, s)
        if not ok then log("setting '" .. tostring(s.id) .. "' of " .. rec.id .. ": " .. tostring(err)) end
      end
    end
  end
  for _, fn in ipairs(pending) do fn() end
  log(string.format("page built in %.0f ms (style %.0f, sync %.0f, rest %.0f)", (os.clock() - t0) * 1000, (t1 - t0) * 1000, (t2 - t1) * 1000, (os.clock() - t2) * 1000))
  for _, ln in ipairs(kmod.perf_sample(false)) do log(ln) end
  local s1, sm1, p1, pm1 = kmod.perf()
  log(string.format("  object scans: %d (%.0f ms); top-level ProcessEvent calls: %d (%.0f ms)", s1 - s0, sm1 - sm0, p1 - p0, pm1 - pm0))
end

function H.tab(self)
  if ui and ui.layout:IsValid() and self == ui.tab then
    local t = os.clock()
    refresh()
    ui.cs:SetActiveWidgetIndex(ui.pageIndex)
    log(string.format("tab click handled in %.0f ms", (os.clock() - t) * 1000))
  end
end

function H.changed(self, kind)
  if not ui or ui.updating then return end
  local info = ui.ctrls and ui.ctrls[self:GetAddress()]
  if not info then return end
  if info.kind == "enable" and kind == "bool" then
    kmod.set_enabled(info.rec.id, self:GetIsChecked())
    log(info.rec.id .. (self:GetIsChecked() and " enabled" or " disabled"))
    kmod.reload_mods()
  elseif info.kind == "bool" and kind == "bool" then
    kmod.settings_set(info.rec, info.id, self:GetIsChecked())
  elseif info.kind == "number" and kind == "number" then
    local v = self.Slider:GetValue()
    kmod.settings_set(info.rec, info.id, v)
    info.row.OptionNameText:SetText(info.label .. " (" .. fmtNumber(v) .. ")")
  elseif info.kind == "choice" and kind == "choice" then
    kmod.settings_set(info.rec, info.id, info.options[self:GetSelectedIndex() + 1])
  elseif info.kind == "string" and kind == "string" then
    local ok, v = pcall(function() return self.EditableTextBox:GetText() end)
    if ok and v ~= nil then
      v = tostring(v)
      if info.maxlen and #v > info.maxlen then v = v:sub(1, info.maxlen) end
      kmod.settings_set(info.rec, info.id, v)
    end
  elseif info.kind == "lang" and kind == "choice" then
    kmod.set_language(info.options[self:GetSelectedIndex() + 1])
    -- rebuild the page, but not inside this dropdown's own event (it would be destroyed while on the stack)
    ExecuteWithDelay(300, function() if ui and ui.layout:IsValid() then refresh() end end)
  end
end

-- one more line in the game's device-info block (bottom right): "kmod <version> <dev|release>"
local function addVersionLine(layout)
  local ver = findWidget(layout, "VersionNumber")        -- the game's own RichTextBlock "1.61.27596"
  local provider = findWidget(layout, "Provider")        -- "APK" line; our line goes right under it
  if not (ver and ver:IsValid() and provider and provider:IsValid()) then log("device info block not found"); return nil end
  local box = provider:GetParent()
  local rich = gp:SpawnObject(richCls, layout.WidgetTree)
  pcall(function() rich.TextStyleSet = ver.TextStyleSet end)               -- same look as the game's lines
  pcall(function() rich.DefaultTextStyle = ver.DefaultTextStyle end)
  pcall(function() rich.Justification = ver.Justification end)
  box:AddChild(rich)
  rich:SetText(string.format("kmod %s %s", kmod.version, kmod.flavor))     -- after parenting, like every text we set
  return rich
end

-- tear down a previous run of this script (hot reload via dofile)
if kmod._mm_ui and kmod._mm_ui.layout:IsValid() then
  pcall(function() if kmod._mm_ui.versionLine then kmod._mm_ui.versionLine:RemoveFromParent() end end)
  pcall(function() kmod._mm_ui.tab:RemoveFromParent() end)
  pcall(function() kmod._mm_ui.page:RemoveFromParent() end)
  log("previous UI removed")
end
kmod._mm_ui = nil

local function inject(layout)
  if ui and ui.layout:IsValid() and ui.layout == layout then return end   -- already injected into this instance
  ui = { layout = layout }
  if not statics() then log("UMG statics not ready"); ui = nil; return end
  local cs = layout.ContentSwitcher
  local top = layout.WBP_NUI_OptionsSelectionColumn.TopVerticalBox
  if not (cs:IsValid() and top:IsValid()) then ui = nil; return end        -- blueprint template, not the live screen
  ui.cs, ui.top, ui.ctrls = cs, top, {}
  ui.tab = create("WBP_NUI_MasterTextButton_C")
  top:AddChild(ui.tab)
  ui.tab:SetText(kmod.t("mods"))
  if not captureStyle() then log("could not read the game's settings page style"); ui = nil; return end
  local tree = layout.WidgetTree
  local canvas = gp:SpawnObject(canvasCls, tree)
  local scale = gp:SpawnObject(scaleCls, tree)
  local cslot = canvas:AddChild(scale)
  cslot:SetLayout(ui.style.layout)                          -- same anchors/offsets as the game's page
  cslot:SetAutoSize(ui.style.autoSize)
  scale:SetStretch(7)                                       -- EStretch::UserSpecified, like the game's page
  local vb = gp:SpawnObject(vboxCls, tree)
  local vslot = scale:AddChild(vb)
  vslot:SetHorizontalAlignment(0); vslot:SetVerticalAlignment(0)   -- Fill/Fill (the default would be centred)
  local sz = gp:SpawnObject(sizeCls, tree)
  local slot = vb:AddChild(sz)
  slot:SetSize({ Value = 1.0, SizeRule = 1 })               -- Fill
  slot:SetHorizontalAlignment(2); slot:SetVerticalAlignment(0)   -- centre horizontally, fill vertically (as the game)
  sz:SetMinDesiredWidth(1500); sz:SetMaxDesiredWidth(1500)
  ui.scroll = gp:SpawnObject(scrollCls, tree)
  pcall(function() ui.scroll.WidgetBarStyle = findWidget(cs:GetChildAt(0), "SettingsScrollBox").WidgetBarStyle end)   -- grey bar like the game's
  sz:AddChild(ui.scroll)
  ui.page, ui.scaleBox = canvas, scale
  cs:AddChild(canvas)
  ui.versionLine = addVersionLine(layout)
  ui.pageIndex = cs:GetChildrenCount() - 1
  kmod._mm_ui = ui
  log("Mods tab added to Options screen (page index " .. ui.pageIndex .. ")")
end
H.inject = inject

-- the tab text follows the language
function kmod.on_language_changed()
  if ui and ui.layout:IsValid() then pcall(function() ui.tab:SetText(kmod.t("mods")) end) end
end

-- hooks are registered once per Lua state; they dispatch through kmod._mmh so a re-run only swaps the handlers
if not kmod._mm_hooked then
  kmod._mm_hooked = true
  RegisterHook(PATH.tab,   function() end, function(self) kmod._mmh.tab(self) end)
  RegisterHook(PATH.check, function() end, function(self) kmod._mmh.changed(self, "bool") end)
  RegisterHook(PATH.slide, function() end, function(self) kmod._mmh.changed(self, "number") end)
  RegisterHook(PATH.drop,  function() end, function(self) kmod._mmh.changed(self, "choice") end)
  RegisterHook(PATH.text,  function() end, function(self) kmod._mmh.changed(self, "string") end)
  RegisterHook("W_OptionsLayout_C:Construct", function() end, function(self) kmod._mmh.inject(self) end)
end

-- hot reload / late start: the Options screen may already exist
for _, l in ipairs(FindAllOf("W_OptionsLayout_C") or {}) do
  local ok, err = pcall(inject, l)
  if not ok then log("inject error: " .. tostring(err)) end
end
