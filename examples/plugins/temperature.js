// tio-gui analysis plugin API 1: one retained log entry, no device side effects.
function transform(input) {
  const match = /temp(?:erature)?\s*[:=]\s*(-?\d+(?:\.\d+)?)/i.exec(input);
  if (!match) return {error: 'No temperature field in this entry'};
  const celsius = Number(match[1]);
  return {celsius, fahrenheit: celsius * 9 / 5 + 32};
}
