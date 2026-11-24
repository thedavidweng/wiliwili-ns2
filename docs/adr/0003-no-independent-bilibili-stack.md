# Maintain Bilibili compatibility through upstream fixes

The port reuses the upstream HTTP, API and result models and takes targeted fixes from maintained forks instead of building a separate Bilibili protocol stack. A purpose-built client was rejected because it would create a second compatibility surface to maintain and would drift toward anti-abuse and authorization bypass behavior that is explicitly out of scope.
