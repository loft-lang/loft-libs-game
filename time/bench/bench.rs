// Copyright (c) 2026 Jurjen Stellingwerff
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// time-reference — the pure-Rust twin of the `time` package's performance pass
// (bench/bench.loft), one file built with `rustc -O` as loft's own bench/ builds its
// references.  It computes the SAME workloads with the SAME arithmetic, in the same order,
// and prints the same rows, hash included: a row whose hash matches the loft build's is a
// like-for-like comparison, and only then is a routine's loft time judged against it
// (@FR-Perf-Weight).  No dependencies and no cleverness — plain idiomatic Rust, the speed
// an industry implementation reaches without effort, which is exactly what the bar should be.
//
//     rustc -O --edition=2021 bench/bench.rs -o bench/.build/stats_rs && bench/.build/stats_rs --n 50
//
// Each routine is a port of its loft original in src/time.loft: Hinnant's civil-date
// arithmetic with loft's truncating `/` and `%` and its floor helpers, `parse` walking the
// whole text once per field as `digits_at` does, and `format_iso` building its text with
// `write!` into a reused String.  Values are i64 like loft's integers.
use std::fmt::Write;
use std::hint::black_box;
use std::time::Instant;

const FNV_OFFSET: i64 = 2166136261;
const FNV_PRIME: i64 = 16777619;
const MASK32: i64 = 0xFFFF_FFFF;

const TABLE: i64 = 512;
const EPOCH_BASE: i64 = -2208988800000;
const EPOCH_STEP: i64 = 12345678901;

const PARSE_BATCH: i64 = 10000;
const FORMAT_BATCH: i64 = 10000;
const DAY_BATCH: i64 = 100000;
const WEEK_BATCH: i64 = 50000;

const MS_PER_DAY: i64 = 86400000;
const MS_PER_HOUR: i64 = 3600000;
const MS_PER_MINUTE: i64 = 60000;
const MS_PER_SECOND: i64 = 1000;

fn fnv_word(h: i64, v: i64) -> i64 {
    let w = v & MASK32;
    let mut r = h;
    for sh in [24, 16, 8, 0] {
        r = ((r ^ ((w >> sh) & 255)) * FNV_PRIME) & MASK32;
    }
    r
}

fn fnv_int(h: i64, v: i64) -> i64 {
    fnv_word(fnv_word(h, (v >> 32) & MASK32), v)
}

fn fnv_text(h: i64, s: &str) -> i64 {
    let mut r = h;
    for &b in s.as_bytes() {
        r = ((r ^ b as i64) * FNV_PRIME) & MASK32;
    }
    r
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

// ── src/time.loft ───────────────────────────────────────────────────

fn floor_div(a: i64, b: i64) -> i64 {
    let q = a / b;
    let r = a % b;
    if r != 0 && ((a < 0) != (b < 0)) { q - 1 } else { q }
}

fn floor_mod(a: i64, b: i64) -> i64 {
    ((a % b) + b) % b
}

fn epoch_day(t: i64) -> i64 {
    floor_div(t, MS_PER_DAY)
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let zz = z + 719468;
    let eshift = if zz >= 0 { zz } else { zz - 146096 };
    let era = floor_div(eshift, 146097);
    let doe = zz - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let yr = if m <= 2 { y + 1 } else { y };
    (yr, m, d)
}

fn days_from_civil(cy: i64, cm: i64, cd: i64) -> i64 {
    let yy = if cm <= 2 { cy - 1 } else { cy };
    let yshift = if yy >= 0 { yy } else { yy - 399 };
    let era = floor_div(yshift, 400);
    let yoe = yy - era * 400;
    let mpart = if cm > 2 { cm - 3 } else { cm + 9 };
    let doy = (153 * mpart + 2) / 5 + cd - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

fn from_ymd(y: i64, mo: i64, d: i64) -> i64 {
    days_from_civil(y, mo, d) * MS_PER_DAY
}

fn digit_value(c: char) -> i64 {
    if ('0'..='9').contains(&c) { c as i64 - '0' as i64 } else { -1 }
}

/// `count` decimal digits at byte index `start` — walking the whole text, as the loft
/// original does.
fn digits_at(s: &str, start: i64, count: i64) -> i64 {
    let mut val = 0i64;
    let mut seen = 0i64;
    for (idx, c) in s.char_indices() {
        let idx = idx as i64;
        if idx >= start && idx < start + count {
            let dv = digit_value(c);
            if dv < 0 {
                return -1;
            }
            val = val * 10 + dv;
            seen += 1;
        }
    }
    if seen == count { val } else { -1 }
}

fn parse(s: &str) -> Option<i64> {
    let n = s.len();
    if n < 10 {
        return None;
    }
    let py = digits_at(s, 0, 4);
    let pmo = digits_at(s, 5, 2);
    let pd = digits_at(s, 8, 2);
    if py < 0 || pmo < 0 || pd < 0 {
        return None;
    }
    let mut pt = from_ymd(py, pmo, pd);
    if n >= 16 {
        let ph = digits_at(s, 11, 2);
        let pmi = digits_at(s, 14, 2);
        if ph >= 0 && pmi >= 0 {
            pt += ph * MS_PER_HOUR + pmi * MS_PER_MINUTE;
            if n >= 19 {
                let psec = digits_at(s, 17, 2);
                if psec >= 0 {
                    pt += psec * MS_PER_SECOND;
                }
            }
        }
    }
    Some(pt)
}

fn day(t: i64) -> i64 {
    civil_from_days(epoch_day(t)).2
}

fn ms_of_day(t: i64) -> i64 {
    floor_mod(t, MS_PER_DAY)
}
fn hour(t: i64) -> i64 {
    ms_of_day(t) / MS_PER_HOUR
}
fn minute(t: i64) -> i64 {
    (ms_of_day(t) / MS_PER_MINUTE) % 60
}
fn second(t: i64) -> i64 {
    (ms_of_day(t) / MS_PER_SECOND) % 60
}
fn weekday(t: i64) -> i64 {
    floor_mod(epoch_day(t) + 3, 7)
}

fn iso_calendar(t: i64) -> (i64, i64) {
    let iday = epoch_day(t);
    let iwd = weekday(t);
    let thursday = iday - iwd + 3;
    let iy = civil_from_days(thursday).0;
    let jan1 = days_from_civil(iy, 1, 1);
    let week = (thursday - jan1) / 7 + 1;
    (iy, week)
}

fn iso_week(t: i64) -> i64 {
    iso_calendar(t).1
}

/// `format_date` + `format_seconds`, as `format_iso` composes them, into `out`.
fn format_iso(t: i64, out: &mut String) {
    out.clear();
    let (y, m, d) = civil_from_days(epoch_day(t));
    let (h, mi, s) = (hour(t), minute(t), second(t));
    write!(out, "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, h, mi, s).unwrap();
}

// ── the rows ────────────────────────────────────────────────────────

fn epochs() -> Vec<i64> {
    (0..TABLE).map(|k| EPOCH_BASE + k * EPOCH_STEP).collect()
}

fn bench_parse(n: i64, ep: &[i64]) -> Row {
    let strs: Vec<String> = ep
        .iter()
        .map(|&e| {
            let mut s = String::new();
            format_iso(e, &mut s);
            s
        })
        .collect();
    let input = black_box(&strs);
    let t0 = Instant::now();
    let mut sink = 0i64;
    for r in 0..n {
        let r = black_box(r);
        for i in 0..PARSE_BATCH {
            sink ^= parse(&input[((r + i) & (TABLE - 1)) as usize]).unwrap_or(0);
        }
        sink = black_box(sink);
    }
    let us = t0.elapsed().as_micros() as i64;
    let mut h = FNV_OFFSET;
    for i in 0..PARSE_BATCH {
        h = fnv_int(h, parse(&strs[(i & (TABLE - 1)) as usize]).unwrap_or(0));
    }
    Row { name: "parse", iters: n, us, px: PARSE_BATCH, hash: h, sink }
}

fn bench_format(n: i64, ep: &[i64]) -> Row {
    let input = black_box(ep);
    let mut s = String::new();
    let t0 = Instant::now();
    let mut sink = 0i64;
    for r in 0..n {
        let r = black_box(r);
        for i in 0..FORMAT_BATCH {
            format_iso(input[((r + i) & (TABLE - 1)) as usize], &mut s);
            sink += s.as_bytes()[18] as i64;
        }
        sink = black_box(sink);
    }
    let us = t0.elapsed().as_micros() as i64;
    let mut h = FNV_OFFSET;
    for i in 0..FORMAT_BATCH {
        format_iso(ep[(i & (TABLE - 1)) as usize], &mut s);
        h = fnv_text(h, &s);
    }
    Row { name: "format_iso", iters: n, us, px: FORMAT_BATCH, hash: h, sink }
}

fn bench_int(n: i64, ep: &[i64], name: &'static str, batch: i64, f: fn(i64) -> i64) -> Row {
    let input = black_box(ep);
    let t0 = Instant::now();
    let mut sink = 0i64;
    for r in 0..n {
        let r = black_box(r);
        for i in 0..batch {
            sink += f(input[((r + i) & (TABLE - 1)) as usize]);
        }
        sink = black_box(sink);
    }
    let us = t0.elapsed().as_micros() as i64;
    let mut h = FNV_OFFSET;
    for i in 0..batch {
        h = fnv_int(h, f(ep[(i & (TABLE - 1)) as usize]));
    }
    Row { name, iters: n, us, px: batch, hash: h, sink }
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
    let ep = epochs();
    println!("routine\titers\tus\tns_op\tpx\tns_px\thash");
    let mut sink = 0i64;
    for row in [
        bench_parse(n, &ep),
        bench_format(n, &ep),
        bench_int(n, &ep, "day", DAY_BATCH, day),
        bench_int(n, &ep, "iso_week", WEEK_BATCH, iso_week),
    ] {
        print_row(&row);
        sink ^= row.sink;
    }
    println!("sink={}", black_box(sink));
}
