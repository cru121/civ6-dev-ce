-- OPTIONAL / UNTESTED / needs Dev CE built with DEVCE_FILE_IO (off by default). io.open-style wrapper over Dev CE's native DevCE_ReadFile / DevCE_WriteFile.
-- The game's own Lua io library was removed in build 26407122 (2026-11-06 security patch). Text mode only, no seek.
-- Paste this block at the top of a gameplay script; it shadows the global GetIO with a working version.
local function GetIO()
    local io = {}
    function io.open(path, mode)
        mode = mode or "r"
        local f = { path = path, pos = 1, w = false }
        if mode:sub(1, 1) == "r" then
            local t = DevCE_ReadFile(path)
            if t == nil then return nil, path .. ": cannot open" end
            f.buf = t
        else
            f.w = true; f.out = {}
            if not DevCE_WriteFile(path, "", mode:sub(1, 1) == "a") then return nil, path .. ": cannot open" end
        end
        function f:flush()
            if self.w and #self.out > 0 then DevCE_WriteFile(self.path, table.concat(self.out), true); self.out = {} end
        end
        function f:write(...)
            for i = 1, select("#", ...) do self.out[#self.out + 1] = tostring((select(i, ...))) end
            return self
        end
        function f:close() self:flush(); return true end
        function f:read(fmt)
            local b = self.buf
            if not b then return nil end
            fmt = fmt or "*l"
            if fmt == "*a" or fmt == "a" then local r = b:sub(self.pos); self.pos = #b + 1; return r end
            if fmt == "*l" or fmt == "l" then
                if self.pos > #b then return nil end
                local e = b:find("\n", self.pos, true)
                local r
                if e then r = b:sub(self.pos, e - 1); self.pos = e + 1 else r = b:sub(self.pos); self.pos = #b + 1 end
                return (r:gsub("\r$", ""))
            end
            if type(fmt) == "number" then
                if self.pos > #b then return nil end
                local r = b:sub(self.pos, self.pos + fmt - 1); self.pos = self.pos + fmt; return r
            end
            return nil
        end
        function f:lines() return function() return self:read("*l") end end
        return f
    end
    return io
end
