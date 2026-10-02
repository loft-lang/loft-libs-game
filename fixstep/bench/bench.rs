// Copyright (c) 2026 Jurjen Stellingwerff
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// fixstep-reference — the pure-Rust twin of the `fixstep` package's performance pass
// (bench/bench.loft), one file built with `rustc -O`.  It computes the SAME workloads with
// the SAME arithmetic, in the same order, and prints the same rows, hash included: a row
// whose hash matches the loft build's is a like-for-like comparison, and only then is a
// routine's loft time judged against it (@FR-Perf-Weight).  No dependencies and no
// cleverness — plain idiomatic Rust, the speed an industry implementation reaches without
// effort, which is exactly what the bar should be.
//
//     rustc -O --edition=2021 bench/bench.rs -o bench/.build/stats_rs && bench/.build/stats_rs --n 2
//
// Each routine is a port of its loft original (src/tick_clock.loft, src/tick_timer.loft,
// src/ease.loft): integers are i64 like loft's, a division that loft discharges with `?? 0`
// is a checked division here, and every branch the library takes is taken here.
// `black_box` guards each op's INPUT (the repetition number) and the sink — never anything
// inside a kernel.
use std::hint::black_box;
use std::time::Instant;

const FNV_OFFSET: i64 = 2166136261;
const FNV_PRIME: i64 = 16777619;

const STEP: i64 = 2000000;
const FRAME_60: i64 = 50000;

const CALLS: i64 = 2500000;
const TIMERS: i64 = 20000;
const TICKS: i64 = 250;
const VALUES: i64 = 4000;
const FRAMES: i64 = 1200;

fn fnv(h0: i64, v: &[i64]) -> i64 {
    let mut h = h0;
    for &x in v {
        let w = x & 0xFFFF_FFFF;
        for sh in [24, 16, 8, 0] {
            h = ((h ^ ((w >> sh) & 255)) * FNV_PRIME) & 0xFFFF_FFFF;
        }
    }
    h
}

struct Row {
    name: &'static str,
    iters: i64,
    us: i64,
    px: i64,
    hash: i64,
    sink: i64,
}

fn print_row(r: &Row) {
    let ns_op = r.us * 1000 / r.iters;
    let ns_px = if r.px > 0 { (r.us * 1000) as f64 / (r.iters * r.px) as f64 } else { 0.0 };
    println!("{}\t{}\t{}\t{}\t{}\t{:.3}\t{:x}", r.name, r.iters, r.us, ns_op, r.px, ns_px, r.hash);
}

fn timed<F: FnMut(i64) -> i64>(n: i64, mut f: F) -> (i64, i64) {
    let t0 = Instant::now();
    let mut sink = 0i64;
    for r in 0..n {
        sink = sink.wrapping_add(f(black_box(r)));
    }
    (t0.elapsed().as_micros() as i64, black_box(sink))
}

// ── tick_clock.loft ─────────────────────────────────────────────────

struct TickClock {
    step: i64,
    banked: i64,
    rate_num: i64,
    rate_den: i64,
    rate_carry: i64,
    pump_at: i64,
    pumped: bool,
}

fn clock_new(step: i64) -> TickClock {
    TickClock { step: step.max(1), banked: 0, rate_num: 1, rate_den: 1, rate_carry: 0,
                pump_at: 0, pumped: false }
}

fn clock_set_rate(clk: &mut TickClock, num: i64, den: i64) {
    clk.rate_num = num.max(0);
    clk.rate_den = den.max(1);
    clk.rate_carry = 0;
}

fn clock_scaled(clk: &mut TickClock, elapsed: i64) -> i64 {
    if clk.rate_den <= 0 {
        return elapsed;
    }
    let total = clk.rate_carry + elapsed * clk.rate_num;
    let sim = total.checked_div(clk.rate_den).unwrap_or(0);
    clk.rate_carry = total - sim * clk.rate_den;
    sim
}

fn clock_advance(clk: &mut TickClock, elapsed: i64) -> i64 {
    if elapsed <= 0 {
        return 0;
    }
    clk.banked += clock_scaled(clk, elapsed);
    let whole = clk.banked.checked_div(clk.step).unwrap_or(0);
    clk.banked -= whole * clk.step;
    whole
}

fn clock_pump(clk: &mut TickClock, now: i64) -> i64 {
    if !clk.pumped {
        clk.pumped = true;
        clk.pump_at = now;
        return 0;
    }
    if now <= clk.pump_at {
        clk.pump_at = now;
        return 0;
    }
    let dt = now - clk.pump_at;
    clk.pump_at = now;
    clock_advance(clk, dt)
}

// ── tick_timer.loft ─────────────────────────────────────────────────

struct Timer {
    spent: i64,
    total: i64,
}

fn timer_new() -> Timer {
    Timer { spent: 0, total: 0 }
}

fn timer_arm(t: &mut Timer, units: i64) {
    t.spent = 0;
    if units <= 0 {
        t.total = 0;
        return;
    }
    t.total = units;
}

fn timer_spent(t: &Timer) -> i64 {
    if t.total <= 0 {
        return 0;
    }
    t.spent
}

fn timer_spend(t: &mut Timer, units: i64) -> bool {
    if t.total <= 0 {
        return false;
    }
    if units <= 0 {
        return false;
    }
    t.spent += units;
    if t.spent < t.total {
        return false;
    }
    t.spent = 0;
    t.total = 0;
    true
}

// ── ease.loft ───────────────────────────────────────────────────────

fn ease_fraction(k: f64, dt: f64) -> f64 {
    if dt <= 0.0 {
        return 0.0;
    }
    1.0 - (-(k * dt)).exp()
}

fn approach(now: f64, want: f64, k: f64, dt: f64) -> f64 {
    now + (want - now) * ease_fraction(k, dt)
}

// ── The rows ────────────────────────────────────────────────────────

fn pump_op(r: i64) -> [i64; 2] {
    let mut clk = clock_new(STEP + (r & 1));
    let mut now = 1000i64;
    let mut steps = 0i64;
    for i in 0..CALLS {
        now += FRAME_60 + (i & 1023) * 7;
        steps += clock_pump(&mut clk, now);
    }
    [steps, clk.banked]
}

fn bench_pump(n: i64) -> Row {
    let (us, sink) = timed(n, |r| pump_op(r)[0]);
    Row { name: "clock_pump", iters: n, us, px: CALLS, hash: fnv(FNV_OFFSET, &pump_op(black_box(0))), sink }
}

fn advance_op(r: i64) -> [i64; 2] {
    let mut clk = clock_new(STEP + (r & 1));
    clock_set_rate(&mut clk, 3, 4);
    let mut steps = 0i64;
    for i in 0..CALLS {
        steps += clock_advance(&mut clk, FRAME_60 + (i & 1023) * 7);
    }
    [steps, clk.banked]
}

fn bench_advance(n: i64) -> Row {
    let (us, sink) = timed(n, |r| advance_op(r)[0]);
    Row { name: "clock_advance", iters: n, us, px: CALLS,
          hash: fnv(FNV_OFFSET, &advance_op(black_box(0))), sink }
}

fn timer_op(r: i64) -> [i64; 2] {
    let mut timers: Vec<Timer> = Vec::new();
    for _ in 0..TIMERS {
        timers.push(timer_new());
    }
    for (j, t) in timers.iter_mut().enumerate() {
        timer_arm(t, 1000 + (j as i64 * 37 + r) % 4001);
    }
    let mut fires = 0i64;
    for k in 0..TICKS {
        let units = 40 + (k & 15) * 3;
        for (j, t) in timers.iter_mut().enumerate() {
            if timer_spend(t, units) {
                fires += 1;
                timer_arm(t, 1000 + (j as i64 * 37 + k) % 4001);
            }
        }
    }
    let mut spent = 0i64;
    for t in &timers {
        spent += timer_spent(t);
    }
    [fires, spent]
}

fn bench_timer(n: i64) -> Row {
    let (us, sink) = timed(n, |r| timer_op(r)[0]);
    Row { name: "timer_spend", iters: n, us, px: TIMERS * TICKS,
          hash: fnv(FNV_OFFSET, &timer_op(black_box(0))), sink }
}

fn approach_op(r: i64) -> Vec<f64> {
    let mut vals: Vec<f64> = Vec::new();
    for i in 0..VALUES {
        vals.push((i as f64) * 0.5 + ((r & 1) as f64));
    }
    for f in 0..FRAMES {
        let dt = 0.016 + ((f & 3) as f64) * 0.0005;
        let lift = (((f >> 6) & 7) as f64) * 10.0;
        for i in 0..VALUES {
            let k = 1.5 + ((i & 15) as f64) * 0.125;
            vals[i as usize] = approach(vals[i as usize], ((i & 255) as f64) + lift, k, dt);
        }
    }
    vals
}

fn bench_approach(n: i64) -> Row {
    let (us, sink) = timed(n, |r| (approach_op(r)[1] * 1000.0) as i64);
    let one = approach_op(black_box(0));
    let ints: Vec<i64> = one.iter().map(|v| (v * 1000000.0) as i64).collect();
    Row { name: "approach", iters: n, us, px: VALUES * FRAMES, hash: fnv(FNV_OFFSET, &ints), sink }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut n: i64 = 20;
    let mut i = 1;
    while i < args.len() {
        if args[i] == "--n" && i + 1 < args.len() {
            n = args[i + 1].parse().unwrap_or(20);
            i += 1;
        }
        i += 1;
    }
    if n < 1 {
        n = 1;
    }
    let t0 = Instant::now();
    println!("routine\titers\tus\tns_op\tpx\tns_px\thash");
    let rows = [bench_pump(n), bench_advance(n), bench_timer(n), bench_approach(n)];
    let mut sink = 0i64;
    for row in &rows {
        print_row(row);
        sink = sink.wrapping_add(row.sink);
    }
    println!("time: {}ms sink={}", t0.elapsed().as_millis(), sink);
}
