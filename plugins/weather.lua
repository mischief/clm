-- plugins/weather.lua
-- Weather plugin using wttr.in (no API key required).
-- Accepts a location (city name, airport code, zip, coordinates, etc.)
-- and returns current conditions.

clm.tool_register("get_weather", {
    description = [[
Get current weather for a location using wttr.in.
If the user does not specify a location or just asks for the weather, use the empty string "".
Accepts city names, airport codes (3 letters), zip/area codes,
or GPS coordinates (e.g. '-78.46,106.79').]],
    params_schema = {
        type = "object",
        properties = {
            location = {
                type = "string",
                description = "city name, airport code, zip code, or "
                    .. "GPS coordinates",
            },
        },
        required = { "location" },
    },
    timeout_ms = 15000,
    invoke = function(args, ctx)
        local loc = args.location or ""
        if loc ~= "" then
            loc = string.gsub(loc, " ", "+")
        end

        local url = "https://wttr.in/" .. loc .. "?format=j1"

        local resp, err = http.get(url)
        if err then
            ctx:fail("HTTP request failed: " .. err)
            return
        end

        if resp.status ~= 200 then
            ctx:fail("wttr.in returned status " .. tostring(resp.status))
            return
        end

        local data = json.decode(resp.body)
        if not data or not data.current_condition then
            ctx:fail("unexpected response format from wttr.in")
            return
        end

        local cc = data.current_condition[1]
        if not cc then
            ctx:fail("no current_condition in response")
            return
        end

        -- Extract the weather description text.
        local desc = "unknown"
        if cc.weatherDesc and cc.weatherDesc[1] then
            desc = cc.weatherDesc[1].value or "unknown"
        end

        -- Get the human-readable location name. The JSON API returns
        -- junk areaNames for IP queries, but format=%l always works.
        local area = args.location
        local loc_url = "https://wttr.in/" .. loc .. "?format=%l"
        local lr, lerr = http.get(loc_url)
        if lr and lr.status == 200 and lr.body and lr.body ~= "" then
            -- Trim trailing whitespace/newline.
            area = string.gsub(lr.body, "%s+$", "")
        end

        local result = string.format(
            "%s: %s\n"
            .. "Temperature: %s°F (feels like %s°F)\n"
            .. "Wind: %s mph %s\n"
            .. "Humidity: %s%%\n"
            .. "Cloud cover: %s%%\n"
            .. "Visibility: %s miles",
            area, desc,
            cc.temp_F or "?", cc.FeelsLikeF or "?",
            cc.windspeedMiles or "?", cc.winddir16Point or "",
            cc.humidity or "?",
            cc.cloudcover or "?",
            cc.visibilityMiles or "?")

        ctx:complete(result)
    end,
})
