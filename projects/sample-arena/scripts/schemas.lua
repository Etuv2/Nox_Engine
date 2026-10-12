-- Component schemas owned by the project (no native ComponentType is involved).
nox.schema("Health", { current = "integer", maximum = "integer" }, { current = 100, maximum = 100 })
nox.schema("Quest", { state = "string", progress = "number" }, { state = "open", progress = 0.0 })
nox.schema("Inventory", { gold = "integer" }, { gold = 0 })