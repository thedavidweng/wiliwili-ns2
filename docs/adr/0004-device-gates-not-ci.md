# Real-console gates cannot be cleared by CI or host tests

G1 and G2 require evidence from a human tester against a specific NRO SHA-256; a blocked gate stays blocked and scope is never downgraded automatically. Compile success, host tests and synthetic screenshots cannot establish runtime compatibility, audio/video sync or glyph coverage. The resulting delay when device time is unavailable is deliberate rather than accidental.
