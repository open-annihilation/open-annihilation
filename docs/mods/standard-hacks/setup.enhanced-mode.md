# Enhanced Mode

<!-- BEGIN GENERATED: facts -->
| Fact | Value |
| --- | --- |
| Hack id | `setup.enhanced-mode` |
| Area | Game Setup (`setup`) |
| Scope | sim: part of the profile hash; every machine must agree |
| Runs on | Every machine in the game. |
| Parameters | 1 |
| Status | implemented |
<!-- END GENERATED: facts -->

## Description

<!-- WRITE: brief description -->

## Configuration example

<!-- WRITE: configuration example -->

## Details

<!-- WRITE: details and implementation notes -->

## Full configuration schema

<!-- BEGIN GENERATED: schema -->
Hack id `setup.enhanced-mode`, written under the profile's `hacks` block.

| Write | Means |
| --- | --- |
| `setup.enhanced-mode: true` | On, every parameter at its default. |
| `setup.enhanced-mode: {version: 1}` | On, the parameters named set and the rest at their defaults. |
| `setup.enhanced-mode: 1` | On, the shorthand: a bare value sets `version`. |
| `setup.enhanced-mode: false` | Off, the same as leaving it out: 3.1c behaviour. |

### Parameters

| Parameter | Type | Unit | Allowed | Length | Baseline (3.1c) | Default | Adjustable | Scope | Overridden by |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `version` | `int` | - | `1` to `1` | - | `1` | `1` | `match` | `sim` | - |

Adjustable: `match`: as `install`, and the host may also change it for one game.

### Presets

This hack has no presets.

### Shorthand

The hack has one parameter, so a bare value sets `version`: `setup.enhanced-mode: 1` is `setup.enhanced-mode: {version: 1}`.

### Every parameter at its default

```yaml
hacks:
  setup.enhanced-mode:
    version: 1
```
<!-- END GENERATED: schema -->
