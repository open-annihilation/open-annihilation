# Weapon execution

`tick_weapons` runs a unit's three weapon slots each tick. It counts reloads
down in the unit's canonical `UnitWeapon.reload`, resolves each target through
the host, starts the `AimPrimary`, `AimSecondary` or `AimTertiary` script of a
turret or vertical-launch slot, applies the reach test, runs the typed
projectile-constructor boundary, and does the game's bookkeeping after a
successful shot.

The reach test receives the unit position; the ballistic reach test keeps its
own preliminary range, water and height gates. Non-stockpile slots compare the
shot cost with the owner's stores (`can_pay_shot_cost`) before the projectile
constructor, reporting `insufficient_resources` and firing nothing when either
falls short, and pay the cost (`pay_shot_cost`) only after the constructor
fired. The turret constructor waits until the Aim script it started has returned
nonzero (`UnitWeapon.aim_ready`) and the aim is within tolerance; turret and
vertical-launch shots clear the aim bit, so the slot aims again before the next
shot. Projectile construction stays host-owned because
`WeaponDef.fire_callback` selects different constructors. A successful
`fire_projectile` must also dispatch the corresponding COB `FireWeapon`
callback before it returns true. Returning success without a projectile and
callback is not a supported integration.

`weapon_launch.hpp` holds the launch, flight and impact arithmetic of the
constructors: bearings, launch speeds and expiry, ballistic lift, accuracy
spread, veteran lead, burst children, guidance steering, damage scaling and the
area blast bookkeeping.

## Interceptors, map contact and retaliation

`interceptor.hpp` covers the interceptor target search, the vertical-launch
constructor with its Aim-script wait and interceptor gate, the Aim return
callback, projectile record initialisation and the guided aim point (including
the cruise descent). `projectile_contact.hpp` covers the per-tick map-cell
contact test: the intercept burst, ground and air occupants, the plot feature,
ground bounce and the water surface. `retaliation.hpp` covers a damaged unit's
reaction to its attacker: the attack-order attempt, weapon-slot re-aiming over
the ballistics reach test, the computer-player alert and the under-attack
notice. All three work on the canonical `World` records; order queues, category
masks, explosions, scripts and the records shared with other players stay with
the caller through plain hooks. `projectile_pool.hpp` covers the pool
allocator, retirement, compaction and the cancelling of a dead unit's burst
spawners. The match runtime flies its projectiles through the pool, contact and
interceptor code; `projectile_plot_contact` takes the plot fields directly
because the match's plots are not canonical yet. The match's damage path runs
retaliation.

The native projectile pool keeps 300 records in the normal mode. Enhanced Mode
may bind up to 32,767 records without changing the packed projectile or game
records. Its match-owned compaction scratch maps old indices to new indices in
linear time, keeping every interceptor attached to its original surviving target
and clearing references to retired targets. The normal mode retains its existing
compaction behavior. Pool usage records the occupied high-water count and rejected
allocations so benchmarks report when the projectile budget constrains a battle.
