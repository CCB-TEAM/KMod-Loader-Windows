-- Tiny JSON codec shared by the main Lua state (kmod.json) and by worker threads (json).
local json = {}
local function skipws(s, i) return s:find("%S", i) or (#s + 1) end
local parseValue
local function parseString(s, i)
  local out, j = {}, i + 1
  local esc = { n = "\n", t = "\t", r = "\r", b = "\b", f = "\f", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }
  while true do
    local c = s:sub(j, j)
    if c == '"' then return table.concat(out), j + 1
    elseif c == "\\" then
      local n = s:sub(j + 1, j + 1)
      if n == "u" then
        local cp = tonumber(s:sub(j + 2, j + 5), 16)
        j = j + 6
        if cp and cp >= 0xD800 and cp < 0xDC00 and s:sub(j, j + 1) == "\\u" then   -- surrogate pair
          local lo = tonumber(s:sub(j + 2, j + 5), 16)
          if lo and lo >= 0xDC00 and lo < 0xE000 then cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); j = j + 6 end
        end
        out[#out + 1] = utf8.char(cp or 63)
      else out[#out + 1] = esc[n] or n; j = j + 2 end
    elseif c == "" then error("unterminated string")
    else out[#out + 1] = c; j = j + 1 end
  end
end
parseValue = function(s, i)
  i = skipws(s, i)
  local c = s:sub(i, i)
  if c == "{" then
    local t = {}
    i = skipws(s, i + 1)
    if s:sub(i, i) == "}" then return t, i + 1 end
    while true do
      i = skipws(s, i)
      local k; k, i = parseString(s, i)
      i = skipws(s, i)
      assert(s:sub(i, i) == ":", "expected ':' at " .. i)
      local v; v, i = parseValue(s, i + 1)
      t[k] = v
      i = skipws(s, i)
      local d = s:sub(i, i); i = i + 1
      if d == "}" then return t, i elseif d ~= "," then error("bad object at " .. i) end
    end
  elseif c == "[" then
    local t = {}
    i = skipws(s, i + 1)
    if s:sub(i, i) == "]" then return t, i + 1 end
    while true do
      local v; v, i = parseValue(s, i)
      t[#t + 1] = v
      i = skipws(s, i)
      local d = s:sub(i, i); i = i + 1
      if d == "]" then return t, i elseif d ~= "," then error("bad array at " .. i) end
    end
  elseif c == '"' then return parseString(s, i)
  elseif s:sub(i, i + 3) == "true" then return true, i + 4
  elseif s:sub(i, i + 4) == "false" then return false, i + 5
  elseif s:sub(i, i + 3) == "null" then return nil, i + 4
  else
    local num = s:match("^-?%d+%.?%d*[eE]?[+-]?%d*", i)
    assert(num and #num > 0, "bad json value at " .. i)
    return tonumber(num), i + #num
  end
end
function json.decode(s)
  if s:sub(1, 3) == "\239\187\191" then s = s:sub(4) end   -- BOM
  local ok, v = pcall(function() return (parseValue(s, 1)) end)
  if ok then return v end
  return nil, (tostring(v):gsub("^.-:%d+: ", ""))
end
local function enc(v, out)
  local t = type(v)
  if t == "table" then
    if #v > 0 or next(v) == nil then
      out[#out + 1] = "["
      for i, x in ipairs(v) do if i > 1 then out[#out + 1] = "," end enc(x, out) end
      out[#out + 1] = "]"
    else
      local keys = {}
      for k in pairs(v) do keys[#keys + 1] = tostring(k) end
      table.sort(keys)
      out[#out + 1] = "{"
      for i, k in ipairs(keys) do
        if i > 1 then out[#out + 1] = "," end
        enc(k, out); out[#out + 1] = ":"; enc(v[k], out)
      end
      out[#out + 1] = "}"
    end
  elseif t == "string" then out[#out + 1] = '"' .. v:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"'
  elseif t == "number" or t == "boolean" then out[#out + 1] = tostring(v)
  else out[#out + 1] = "null" end
end
function json.encode(v) local out = {}; enc(v, out); return table.concat(out) end
return json
