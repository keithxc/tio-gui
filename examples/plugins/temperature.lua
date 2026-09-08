-- tio-gui analysis plugin API 1. Return UTF-8 text.
function transform(input)
  local temperature = tonumber(input:match('temp%s*[:=]%s*([%-%.%d]+)'))
  if not temperature then return 'No temp field in this entry' end
  return string.format('Celsius: %.2f; Fahrenheit: %.2f', temperature, temperature * 9 / 5 + 32)
end
