-- Automation 4 demo script
-- Macro that uppercases the first letter in each selected line

local tr = aegisub.gettext

script_name = tr"Capitalize first letter"
script_description = tr"Uppercase the first letter in each selected line"
script_author = "Pencil"
script_version = "1"

include("unicode.lua")

local function capitalize_first_letter(text)
	local in_tags = false
	local escaped = false
	local converted = false
	local out = {}

	for c in unicode.chars(text) do
		if escaped then
			out[#out + 1] = c
			escaped = false
		elseif in_tags then
			out[#out + 1] = c
			if c == "}" then
				in_tags = false
			end
		else
			if c == "{" then
				in_tags = true
				out[#out + 1] = c
			elseif c == "\\" then
				escaped = true
				out[#out + 1] = c
			elseif not converted then
				local upper = unicode.to_upper_case(c)
				local lower = unicode.to_lower_case(c)
				if upper ~= lower then
					out[#out + 1] = upper
					converted = true
				else
					out[#out + 1] = c
				end
			else
				out[#out + 1] = c
			end
		end
	end

	return table.concat(out)
end

function capitalize_selected_lines(subtitles, selected_lines, active_line)
	for _, i in ipairs(selected_lines) do
		local line = subtitles[i]
		line.text = capitalize_first_letter(line.text)
		subtitles[i] = line
	end
	aegisub.set_undo_point(script_name)
end

aegisub.register_macro(script_name, script_description, capitalize_selected_lines)
