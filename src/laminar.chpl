use CTypes;

extern proc sluice_init(queue_num: c_ushort): c_int;
extern proc sluice_get_pressure_bytes(): c_longlong;
extern proc sluice_get_inflow_bytes_and_reset(): c_longlong;
extern proc sluice_set_release_budget_bytes(bytes: c_longlong): void;
extern proc sluice_shutdown(): void;

extern proc tick_init_1khz(): void;
extern proc tick_wait_next(): void;

inline proc clamp(x: real, lo: real, hi: real): real {
if x < lo then return lo;
if x > hi then return hi;
return x;
}

proc main() {
// ---- Constants (EXACT) ----
const N: int = 50;
const DT: real = 0.001; // 1000 Hz
const dx: real = 1.0 / (N:real - 1.0);
const invDx: real = 1.0 / dx;
const invDx2: real = 1.0 / (dx*dx);

const B: real = 32.0 * 1024.0 * 1024.0; // reservoir capacity bytes
const RHO_TARGET: real = 0.35;
const U_MAX: real = 18.0;

// ---- State ----
var nu: real = 0.02; // start viscosity
var u: [0..N-1] real = 0.0;
var uNext: [0..N-1] real = 0.0;

writeln("Laminar: initializing sluice (NFQUEUE)...");
const rc = sluice_init(0:c_ushort);
if rc != 0 then {
writeln("ERROR: sluice_init failed rc=", rc);
return;
}

tick_init_1khz();
writeln("Laminar: running 1000 Hz solver loop. (Logs at 10 Hz)");

var tickCount: int = 0;

// Main loop
while true {
// 1) Sample sensors
const pressureBytes = sluice_get_pressure_bytes(): real;
const inflowBytes = sluice_get_inflow_bytes_and_reset(): real;

const rho = clamp(pressureBytes / B, 0.0, 1.0);
const u_in = clamp(inflowBytes / (B * DT), 0.0, U_MAX);

// 2) Boundary condition at inlet
u[0] = u_in;

// 3) Viscous Burgers update (upwind convection, central diffusion)
//    Sequential loop to avoid 1ms parallel task launch overhead
for i in 1..N-2 {
  const ui = u[i];

  // Upwind du/dx depending on sign of ui (ui is non-negative here, but keep exact)
  const dudx =
    if ui >= 0.0
    then (u[i] - u[i-1]) * invDx
    else (u[i+1] - u[i]) * invDx;

  const d2udx2 = (u[i+1] - 2.0*ui + u[i-1]) * invDx2;

  // u_t + u u_x = nu u_xx  => uNext = u + DT*( -u*dudx + nu*d2udx2 )
  uNext[i] = ui + DT * ( -ui * dudx + nu * d2udx2 );
}

// 4) Outlet boundary (zero-gradient)
uNext[N-1] = uNext[N-2];
uNext[0] = u_in;

// 5) Swap
u = uNext;

// 6) Compute u_exit and Reynolds number (normalized L=1 => Re = u_exit/nu)
const u_exit = clamp(u[N-1], 0.0, U_MAX);
const nuSafe = clamp(nu, 1e-6, 1.0);
const Re = u_exit / nuSafe;

// 7) Viscosity control (EXACT)
const nuTarget = clamp(u_exit / 2000.0, 1e-4, 1e-1);
nu = 0.95*nu + 0.05*nuTarget;

// 8) Pressure term and command outflow
const u_pressure = 12.0 * (rho - RHO_TARGET);
const u_cmd = clamp(u_exit + u_pressure, 0.0, U_MAX);

// 9) Budget mapping (bytes per tick)
const budget = (u_cmd * B * DT): real;
const budgetBytes: int(64) = if budget < 0.0 then 0 else budget: int(64);

sluice_set_release_budget_bytes(budgetBytes: c_longlong);

// 10) Log at 10 Hz
tickCount += 1;
if tickCount % 100 == 0 {
  const regime = if Re > 2000.0 then "[TURBULENT]" else "[LAMINAR]";
  writeln(regime,
          " rho=", rho,
          " u_in=", u_in,
          " u_exit=", u_exit,
          " u_cmd=", u_cmd,
          " nu=", nu,
          " Re=", Re,
          " pressure_bytes=", pressureBytes: int(64),
          " budget_bytes=", budgetBytes);
}

// 11) Wait for next tick
tick_wait_next();

}

// Unreachable in this minimal loop; left for completeness
sluice_shutdown();
}
