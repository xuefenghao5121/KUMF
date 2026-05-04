#!/usr/bin/env python3
"""
KUMF Phase 2: ARM SPE/PEBS Analysis Engine + AOL Scoring + SOAR Migration Decision

Two analysis modes:
  Mode A (object-level): SPE + prof -> match VA to allocations -> per-object AOL
  Mode B (page-level):  SPE/PEBS -> align to 4KB pages -> per-page AOL (no prof needed)

AOL Formula (from paper):
  AOL(obj) = sum(latency_i / MLP_window) / N_accesses
  Simplified: AOL = LLC_miss_ratio / MLP_approx * access_weight

SVE Awareness:
  - SVE vector loads have natural high MLP (one instruction -> multiple cache lines)
  - Detect SVE patterns via instruction PC range or symbol name
  - Adjust MLP estimate: MLP_sve = MLP_measured * sve_width_factor

Usage (kunpeng SPE):
  perf record -e arm_spe_0/Load+Store+min_latency=32/ -C 0-39 -o spe.data -- ./workload
  perf script -i spe.data > spe.txt
  python3 soar_analyzer.py --spe spe.txt --pmu pmu.txt --mode page --output aol.csv
"""

import argparse
import math
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional, Dict, List, Tuple


# ============================================================
# Data Structures
# ============================================================

@dataclass
class SampleRecord:
    timestamp: float = 0.0
    pid: int = 0
    tid: int = 0
    cpu: int = 0
    va: int = 0
    pc: int = 0
    event_type: str = ""
    weight: int = 0
    symbol: str = ""
    dso: str = ""
    is_sve: bool = False

@dataclass
class AllocRecord:
    timestamp: int = 0
    size: int = 0
    addr: int = 0
    alloc_type: int = 0
    callchain: List[str] = field(default_factory=list)
    caller_symbol: str = ""

@dataclass
class PageInfo:
    page_addr: int = 0
    loads: int = 0
    stores: int = 0
    llc_misses: int = 0
    l1d_misses: int = 0
    memory_accesses: int = 0
    latency_sum: int = 0
    latency_max: int = 0
    sve_accesses: int = 0
    symbols: set = field(default_factory=set)
    total_accesses: int = 0
    avg_latency: float = 0.0
    llc_miss_ratio: float = 0.0
    mlp_approx: float = 0.0
    sve_ratio: float = 0.0
    aol_score: float = 0.0
    tier: str = ""

@dataclass
class ObjectInfo:
    name: str = ""
    total_size: int = 0
    pages: set = field(default_factory=set)
    loads: int = 0
    stores: int = 0
    llc_misses: int = 0
    l1d_misses: int = 0
    memory_accesses: int = 0
    sve_accesses: int = 0
    symbols: set = field(default_factory=set)
    total_accesses: int = 0
    llc_miss_ratio: float = 0.0
    mlp_approx: float = 0.0
    sve_ratio: float = 0.0
    aol_score: float = 0.0
    tier: str = ""

@dataclass
class PMUStats:
    bus_access: int = 0
    llc_loads: int = 0
    llc_load_misses: int = 0
    l1d_cache: int = 0
    l1d_miss: int = 0
    stalled_backend: int = 0
    cycles: int = 0
    instructions: int = 0

    @property
    def mlp_global(self) -> float:
        if self.stalled_backend > 0:
            return self.bus_access / self.stalled_backend
        return 1.0

    @property
    def llc_miss_rate_global(self) -> float:
        if self.llc_loads > 0:
            return self.llc_load_misses / self.llc_loads
        return 0.0


# ============================================================
# Parsers
# ============================================================

class SPEParser:
    """Parse ARM SPE perf script output"""
    ARM_PATTERN = re.compile(
        r'\s*(\S+)\s+(\d+)\s+\[(\d+)\]\s+([\d.]+):\s+(\d+)\s+'
        r'([\w\-]+):\s+([0-9a-fA-F]+)\s+(.+?)\s*\((.+?)\)'
    )

    @staticmethod
    def detect_format(filepath: str) -> str:
        with open(filepath, 'r', errors='replace') as f:
            for _, line in zip(range(200), f):
                if 'arm_spe' in line or 'l1d-access' in line or 'llc-access' in line:
                    return 'arm_spe'
                if 'cpu_core/mem-loads' in line or 'mem-stores' in line or 'mem-loads-aux' in line:
                    return 'x86_pebs'
        return 'unknown'

    @classmethod
    def parse(cls, filepath: str, fmt: str = 'auto') -> Tuple[List[SampleRecord], int]:
        if fmt == 'auto':
            fmt = cls.detect_format(filepath)
        if fmt == 'arm_spe':
            return cls._parse_arm(filepath)
        elif fmt == 'x86_pebs':
            return cls._parse_pebs(filepath)
        else:
            return cls._parse_arm(filepath)

    @classmethod
    def _parse_arm(cls, filepath: str) -> Tuple[List[SampleRecord], int]:
        records = []
        errors = 0
        with open(filepath, 'r', errors='replace') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                m = cls.ARM_PATTERN.match(line)
                if not m:
                    errors += 1
                    continue
                try:
                    event = cls._normalize_event(m.group(6))
                    va = int(m.group(7), 16)
                    sym = m.group(8).strip()
                    dso = m.group(9).strip()
                    rec = SampleRecord(
                        pid=int(m.group(2)), tid=int(m.group(2)),
                        cpu=int(m.group(3)), timestamp=float(m.group(4)),
                        va=va, event_type=event, symbol=sym, dso=dso,
                        is_sve=cls._detect_sve(sym),
                    )
                    records.append(rec)
                except (ValueError, IndexError):
                    errors += 1
        return records, errors

    @classmethod
    def _parse_pebs(cls, filepath: str) -> Tuple[List[SampleRecord], int]:
        """Parse x86 PEBS perf script output from 'perf mem record'

        Format: comm TID [CPU] TS: WEIGHT EVENT: VA EXTRA |OP LOAD/STORE|LVL ...|SYM (DSO)
        Example:
          gmx 1234 [012] 123.456: 2559 cpu_core/mem-loads/P: 709174661190 ... |OP LOAD|LVL L1 hit|...
        """
        records = []
        errors = 0
        with open(filepath, 'r', errors='replace') as f:
            for line_no, line in enumerate(f, 1):
                line = line.strip()
                if not line or line.startswith('#') or line.startswith('Only'):
                    continue

                parts = line.split()
                if len(parts) < 8:
                    errors += 1
                    continue
                try:
                    tid = int(parts[1])
                    cpu = int(parts[2].strip('[]'))
                    ts = float(parts[3].rstrip(':'))
                    weight = int(parts[4]) if parts[4].isdigit() else 0

                    # Detect event type
                    event_field = parts[5] if len(parts) > 5 else ""
                    if 'mem-loads' in event_field or 'mem-load' in event_field:
                        event_type = 'load'
                    elif 'mem-stores' in event_field or 'mem-store' in event_field:
                        event_type = 'store'
                    else:
                        event_type = 'unknown'

                    # Data address: after the event field (field after ':')
                    # Format: EVENT: VA  (the ':' is part of field 5, VA is field 6)
                    va = 0
                    va_str = parts[6] if len(parts) > 6 else ""
                    if va_str:
                        try:
                            va = int(va_str, 16)
                        except ValueError:
                            try:
                                va = int(va_str)
                            except ValueError:
                                errors += 1
                                continue
                    if va == 0:
                        errors += 1
                        continue

                    # Parse LVL field from the line to determine cache level
                    # Format: ...|LVL L3 miss|... or ...|LVL L1 hit|...
                    lvl_match = re.search(r'\|LVL\s+([^|]+)\|', line)
                    llc_miss = False
                    dram_access = False
                    if lvl_match:
                        lvl_str = lvl_match.group(1).strip()
                        if 'L3 miss' in lvl_str or 'RAM' in lvl_str or 'Remote' in lvl_str:
                            llc_miss = True
                        if 'RAM' in lvl_str or 'Remote' in lvl_str:
                            dram_access = True

                    # Latency: weight field or from ldlat
                    # NOTE: perf mem weight field is sample weight, NOT latency.
                    # Actual latency info is in the LVL field and the numeric fields after LVL.
                    # For mem-loads with ldlat, there may be a latency value, but not in weight.
                    latency_ns = 0  # Do NOT use weight as latency

                    # Symbol and DSO: last fields in parens
                    sym = ""
                    dso = ""
                    # Find last '(' for DSO
                    for i in range(len(parts) - 1, 6, -1):
                        if '(' in parts[i]:
                            dso = parts[i].strip('()')
                            # Symbol is the field before DSO
                            if i > 7:
                                sym = parts[i-1]
                            break

                    # Update event type based on LLC info
                    if llc_miss:
                        event_type = 'llc_miss'
                    elif dram_access:
                        event_type = 'dram'

                    rec = SampleRecord(
                        pid=tid, tid=tid, cpu=cpu, timestamp=ts,
                        va=va, weight=latency_ns, event_type=event_type,
                        symbol=sym, dso=dso,
                    )
                    records.append(rec)
                except (ValueError, IndexError):
                    errors += 1
        return records, errors

    @staticmethod
    def _normalize_event(event: str) -> str:
        mapping = {
            'l1d-access': 'l1d_hit', 'l1d-miss': 'l1d_miss',
            'l2d-access': 'l2_hit', 'l2d-miss': 'l2_miss',
            'llc-access': 'llc_hit', 'llc-miss': 'llc_miss',
            'memory': 'dram', 'branch-miss': 'branch_miss',
            'tlb-access': 'tlb_hit', 'tlb-miss': 'tlb_miss',
        }
        return mapping.get(event, event)

    @staticmethod
    def _detect_sve(symbol: str) -> bool:
        """Detect ARM SVE instructions (NOT NEON).
        SVE: ld1w/st1w/ld1d/st1d/ld2w/st2w/ldff1/stnt1...
        NEON (vst/vld): these are NOT SVE, exclude them.
        """
        sve_only = ['ld1w', 'st1w', 'ld1d', 'st1d', 'ld2w', 'st2w',
                     'ldff1', 'stnt1', 'ld1b', 'st1b', 'ld1h', 'st1h',
                     'ldnf1', 'ld1q', 'st1q', 'prf', 'cntp']
        return any(h in symbol for h in sve_only)


class ProfParser:
    @staticmethod
    def parse(filepath: str, min_size: int = 0) -> List[AllocRecord]:
        records = []
        try:
            with open(filepath, 'r', errors='replace') as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) < 6:
                        continue
                    try:
                        size = int(parts[4])
                        if size < min_size:
                            continue
                        addr = int(parts[5], 16)
                        rec = AllocRecord(
                            timestamp=int(parts[2]) if parts[2].isdigit() else 0,
                            size=size, addr=addr,
                            alloc_type=int(parts[3]) if parts[3].isdigit() else 1,
                            callchain=parts[6:],
                        )
                        if rec.callchain:
                            for cc in rec.callchain:
                                if '(' in cc:
                                    rec.caller_symbol = cc.split('(')[1].split('+')[0]
                                    break
                        records.append(rec)
                    except (ValueError, IndexError):
                        continue
        except FileNotFoundError:
            pass
        return records


class PMUParser:
    EVENT_MAP = {
        r'bus_access': 'bus_access',
        r'LLC-loads': 'llc_loads',
        r'LLC-load-misses': 'llc_load_misses',
        r'l1d_cache(?!_)': 'l1d_cache',
        r'l1d_cache_lmiss_rd': 'l1d_miss',
        r'stalled-cycles-backend': 'stalled_backend',
        r'(?<!\w)cycles(?!\w)': 'cycles',
        r'instructions': 'instructions',
    }

    @classmethod
    def parse(cls, filepath: str) -> PMUStats:
        pmu = PMUStats()
        try:
            with open(filepath, 'r') as f:
                for line in f:
                    for pattern, attr in cls.EVENT_MAP.items():
                        if re.search(pattern, line):
                            val_str = line.strip().split()[0].replace(',', '')
                            try:
                                setattr(pmu, attr, int(float(val_str)))
                            except ValueError:
                                pass
                            break
        except FileNotFoundError:
            pass
        return pmu


# ============================================================
# Analysis Engines
# ============================================================

class PageAnalyzer:
    PAGE_SHIFT = 12

    def __init__(self, pmu: PMUStats):
        self.pmu = pmu

    def analyze(self, records: List[SampleRecord], target_pid: int = 0) -> Dict[int, PageInfo]:
        pages: Dict[int, PageInfo] = {}
        for rec in records:
            if target_pid > 0 and rec.pid != target_pid:
                continue
            if rec.va == 0:
                continue
            page_key = rec.va >> self.PAGE_SHIFT
            if page_key not in pages:
                pages[page_key] = PageInfo(page_addr=page_key << self.PAGE_SHIFT)
            p = pages[page_key]
            # Count by type - handle both ARM SPE and x86 PEBS events
            if rec.event_type in ('load', 'l1d_hit', 'l2_hit', 'llc_hit'):
                p.loads += 1
            elif rec.event_type == 'store':
                p.stores += 1
            elif rec.event_type == 'llc_miss':
                p.loads += 1
                p.llc_misses += 1
            elif rec.event_type == 'dram':
                p.loads += 1
                p.llc_misses += 1
                p.memory_accesses += 1
            # ARM SPE specific
            if 'miss' in rec.event_type and 'l1d' in rec.event_type:
                p.l1d_misses += 1
            if 'llc_miss' in rec.event_type and rec.event_type not in ('llc_miss',):
                p.llc_misses += 1
            if rec.event_type == 'dram':
                p.memory_accesses += 1
            if rec.weight > 0:
                p.latency_sum += rec.weight
                p.latency_max = max(p.latency_max, rec.weight)
            if rec.is_sve:
                p.sve_accesses += 1
            if rec.symbol:
                p.symbols.add(rec.symbol)
        for p in pages.values():
            self._score_page(p)
        return pages

    def _score_page(self, p: PageInfo):
        p.total_accesses = p.loads + p.stores
        if p.total_accesses == 0:
            return
        # LLC miss ratio — based on loads only (stores don't have LVL info in PEBS)
        if p.loads > 0:
            p.llc_miss_ratio = (p.llc_misses + p.memory_accesses) / p.loads
        elif p.latency_sum > 0:
            p.avg_latency = p.latency_sum / p.total_accesses
            # Platform-adaptive latency threshold for LLC miss estimation
            # x86 L3 miss ~100ns, ARM Kunpeng L3 miss ~50-80ns
            # Use dynamic threshold: if avg_latency > 3x typical L1 hit (~5ns), likely cache miss
            llc_miss_threshold = 50.0  # conservative, works for both x86 and ARM
            p.llc_miss_ratio = min(1.0, p.avg_latency / llc_miss_threshold)
        # MLP
        if p.llc_misses > 0:
            p.mlp_approx = max(1.0, p.memory_accesses / p.llc_misses)
        else:
            p.mlp_approx = self.pmu.mlp_global
        # SVE ratio
        p.sve_ratio = p.sve_accesses / p.total_accesses
        # Effective MLP with SVE adjustment
        effective_mlp = p.mlp_approx * (1.0 + p.sve_ratio * 2.0)
        # AOL = (llc_miss_ratio / effective_MLP) * log2(access_count)
        access_weight = math.log2(max(1, p.total_accesses))
        if effective_mlp > 0:
            p.aol_score = (p.llc_miss_ratio / effective_mlp) * access_weight
        else:
            p.aol_score = access_weight
        # Latency bonus (PEBS with weight)
        if p.avg_latency > 0:
            p.aol_score *= (1 + p.avg_latency / 100.0)


class ObjectAnalyzer:
    def __init__(self, pmu: PMUStats):
        self.pmu = pmu
        self.alloc_ranges: List[Tuple[int, int, str]] = []

    def load_allocations(self, allocs: List[AllocRecord]):
        self.alloc_ranges = []
        for a in allocs:
            if a.size > 0 and a.alloc_type in (1, 2, 3):
                name = a.caller_symbol if a.caller_symbol else f"obj_{a.addr:#x}"
                self.alloc_ranges.append((a.addr, a.addr + a.size, name))
        self.alloc_ranges.sort()

    def analyze(self, records: List[SampleRecord], target_pid: int = 0) -> Dict[str, ObjectInfo]:
        objects: Dict[str, ObjectInfo] = {}
        for rec in records:
            if target_pid > 0 and rec.pid != target_pid:
                continue
            if rec.va == 0:
                continue
            obj_name = self._find_object(rec.va)
            if not obj_name:
                continue
            if obj_name not in objects:
                objects[obj_name] = ObjectInfo(name=obj_name)
            obj = objects[obj_name]
            obj.pages.add(rec.va >> 12)
            if rec.event_type in ('load', 'l1d_hit', 'l2_hit', 'llc_hit'):
                obj.loads += 1
            elif rec.event_type == 'store':
                obj.stores += 1
            if 'llc_miss' in rec.event_type:
                obj.llc_misses += 1
            if rec.event_type == 'dram':
                obj.memory_accesses += 1
            if rec.is_sve:
                obj.sve_accesses += 1
            if rec.symbol:
                obj.symbols.add(rec.symbol)
        for obj in objects.values():
            self._score_object(obj)
        return objects

    def _find_object(self, va: int) -> Optional[str]:
        lo, hi = 0, len(self.alloc_ranges) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            start, end, name = self.alloc_ranges[mid]
            if va < start:
                hi = mid - 1
            elif va >= end:
                lo = mid + 1
            else:
                return name
        return None

    def _score_object(self, obj: ObjectInfo):
        obj.total_accesses = obj.loads + obj.stores
        if obj.total_accesses == 0:
            return
        total_cache = obj.l1d_misses + obj.llc_misses + obj.memory_accesses
        if total_cache > 0:
            obj.llc_miss_ratio = (obj.llc_misses + obj.memory_accesses) / total_cache
        if obj.llc_misses > 0:
            obj.mlp_approx = max(1.0, obj.memory_accesses / obj.llc_misses)
        else:
            obj.mlp_approx = self.pmu.mlp_global
        obj.sve_ratio = obj.sve_accesses / obj.total_accesses
        effective_mlp = obj.mlp_approx * (1 + obj.sve_ratio * 2.0)
        access_weight = math.log2(max(1, obj.total_accesses))
        if effective_mlp > 0:
            obj.aol_score = (obj.llc_miss_ratio / effective_mlp) * access_weight


# ============================================================
# Tier Classification
# ============================================================

class TierClassifier:
    @staticmethod
    def classify_pages(pages: Dict[int, PageInfo], fast_pct: float = 0.2, slow_pct: float = 0.5):
        sorted_items = sorted(pages.items(), key=lambda x: x[1].aol_score, reverse=True)
        total = len(sorted_items)
        fast_end = max(1, int(total * fast_pct))
        slow_start = int(total * (1 - slow_pct))
        for i, (_, page) in enumerate(sorted_items):
            if i < fast_end:
                page.tier = "FAST"
            elif i >= slow_start:
                page.tier = "SLOW"
            else:
                page.tier = "MEDIUM"

    @staticmethod
    def classify_objects(objects: Dict[str, ObjectInfo], fast_pct: float = 0.3):
        sorted_items = sorted(objects.items(), key=lambda x: x[1].aol_score, reverse=True)
        total = len(sorted_items)
        fast_end = max(1, int(total * fast_pct))
        for i, (_, obj) in enumerate(sorted_items):
            if obj.aol_score == 0:
                obj.tier = "SLOW"
            elif i < fast_end:
                obj.tier = "FAST"
            elif obj.aol_score > 0.01:
                obj.tier = "MEDIUM"
            else:
                obj.tier = "SLOW"


# ============================================================
# Output
# ============================================================

def print_report(pages, objects, pmu, total_samples, parse_errors):
    print("\n" + "=" * 70)
    print("  KUMF Phase 2: SOAR Analysis Report")
    print("=" * 70)
    print(f"\n--- Input Stats ---")
    print(f"  Total samples: {total_samples:,} ({parse_errors} parse errors)")
    print(f"  Unique pages: {len(pages):,}")
    print(f"  Identified objects: {len(objects):,}")
    print(f"  Global MLP: {pmu.mlp_global:.4f}")
    print(f"  Global LLC miss rate: {pmu.llc_miss_rate_global:.4f}")

    if pages:
        fast_p = sum(1 for p in pages.values() if p.tier == "FAST")
        med_p = sum(1 for p in pages.values() if p.tier == "MEDIUM")
        slow_p = sum(1 for p in pages.values() if p.tier == "SLOW")
        fast_acc = sum(p.total_accesses for p in pages.values() if p.tier == "FAST")
        total_acc = sum(p.total_accesses for p in pages.values()) or 1

        print(f"\n--- Page Tier Distribution ---")
        print(f"  FAST:  {fast_p:>5} pages ({100*fast_p/len(pages):.1f}%) - {100*fast_acc/total_acc:.1f}% accesses")
        print(f"  MEDIUM:{med_p:>5} pages ({100*med_p/len(pages):.1f}%)")
        print(f"  SLOW:  {slow_p:>5} pages ({100*slow_p/len(pages):.1f}%)")
        print(f"  SVE-heavy pages: {sum(1 for p in pages.values() if p.sve_ratio > 0.3)}")

        sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
        print(f"\n--- Top 20 HOT Pages -> FAST tier ---")
        print(f"  {'Page Addr':<20} {'Total':>6} {'Loads':>6} {'Stores':>6} {'LLC_miss':>8} {'MLP':>6} {'SVE%':>5} {'AOL':>10}")
        print("  " + "-" * 80)
        for p in sorted_pages[:20]:
            print(f"  {p.page_addr:#018x} {p.total_accesses:>6} {p.loads:>6} {p.stores:>6} "
                  f"{p.llc_miss_ratio:>8.4f} {p.mlp_approx:>6.2f} {p.sve_ratio:>5.2f} {p.aol_score:>10.2f}")

    if objects:
        print(f"\n--- Object Tier Distribution ---")
        print(f"  FAST: {sum(1 for o in objects.values() if o.tier=='FAST')}, "
              f"MEDIUM: {sum(1 for o in objects.values() if o.tier=='MEDIUM')}, "
              f"SLOW: {sum(1 for o in objects.values() if o.tier=='SLOW')}")

        sorted_obj = sorted(objects.values(), key=lambda o: o.aol_score, reverse=True)
        print(f"\n--- Top 15 Objects by AOL ---")
        print(f"  {'Object':<40} {'Pages':>6} {'Accesses':>8} {'LLC_miss%':>10} {'SVE%':>5} {'AOL':>10}")
        print("  " + "-" * 85)
        for o in sorted_obj[:15]:
            print(f"  {o.name[:40]:<40} {len(o.pages):>6} {o.total_accesses:>8} "
                  f"{o.llc_miss_ratio:>10.4f} {o.sve_ratio:>5.2f} {o.aol_score:>10.4f}")

    # SOAR migration plan
    if pages:
        sorted_pages = sorted(pages.values(), key=lambda p: p.aol_score, reverse=True)
        print(f"\n--- SOAR Migration Plan ---")
        print(f"  # Hot pages -> FAST tier (local NUMA node)")
        for p in sorted_pages[:10]:
            print(f"  migrate_page {p.page_addr:#018x} -> FAST  # AOL={p.aol_score:.2f} acc={p.total_accesses}")
        print(f"  # Cold pages -> SLOW tier (remote NUMA node)")
        for p in sorted_pages[-5:]:
            print(f"  migrate_page {p.page_addr:#018x} -> SLOW  # AOL={p.aol_score:.2f} acc={p.total_accesses}")


def write_csv(pages, objects, output_path):
    with open(output_path, 'w') as f:
        f.write("type,identifier,loads,stores,total,llc_miss_ratio,mlp_approx,sve_ratio,aol_score,tier\n")
        for p in sorted(pages.values(), key=lambda p: p.aol_score, reverse=True):
            f.write(f"page,{p.page_addr:#018x},{p.loads},{p.stores},{p.total_accesses},"
                    f"{p.llc_miss_ratio:.6f},{p.mlp_approx:.4f},{p.sve_ratio:.4f},"
                    f"{p.aol_score:.4f},{p.tier}\n")
        for o in sorted(objects.values(), key=lambda o: o.aol_score, reverse=True):
            f.write(f"object,{o.name},{o.loads},{o.stores},{o.total_accesses},"
                    f"{o.llc_miss_ratio:.6f},{o.mlp_approx:.4f},{o.sve_ratio:.4f},"
                    f"{o.aol_score:.4f},{o.tier}\n")
    print(f"\nCSV saved: {output_path}")


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description='KUMF SOAR Analyzer - SPE/PEBS + AOL + Tier Decision')
    parser.add_argument('--spe', required=True, help='perf script output (SPE or PEBS)')
    parser.add_argument('--prof', nargs='*', help='prof data.raw files (for object mode)')
    parser.add_argument('--pmu', help='perf stat PMU output file')
    parser.add_argument('--mode', choices=['page', 'object', 'both'], default='page',
                        help='Analysis mode: page-level, object-level, or both')
    parser.add_argument('--output', default='soar_aol.csv', help='Output CSV path')
    parser.add_argument('--pid', type=int, default=0, help='Filter by PID (0=all)')
    parser.add_argument('--min-alloc-size', type=int, default=4096, help='Min allocation size for object matching')
    args = parser.parse_args()

    print("=" * 70)
    print("  KUMF Phase 2: SOAR Analysis Engine")
    print("=" * 70)

    # 1. Parse SPE/PEBS
    print("\n[Step 1] Parse memory access samples...")
    records, errors = SPEParser.parse(args.spe)
    total_raw = len(records) + errors
    if args.pid > 0:
        records = [r for r in records if r.pid == args.pid]
        print(f"  Filtered to PID {args.pid}: {len(records)} records")
    print(f"  Parsed: {len(records):,} valid / {total_raw:,} total ({errors} errors)")

    # 2. Parse PMU
    pmu = PMUStats()
    if args.pmu:
        print("\n[Step 2] Parse PMU stats...")
        pmu = PMUParser.parse(args.pmu)
        print(f"  bus_access: {pmu.bus_access:,}")
        print(f"  LLC-loads: {pmu.llc_loads:,}, LLC-misses: {pmu.llc_load_misses:,}")
        print(f"  MLP_approx: {pmu.mlp_global:.4f}")
    else:
        print("\n[Step 2] No PMU file, using defaults")

    # 3. Analyze
    pages = {}
    objects = {}

    if args.mode in ('page', 'both'):
        print("\n[Step 3a] Page-level analysis...")
        pa = PageAnalyzer(pmu)
        pages = pa.analyze(records, args.pid)
        TierClassifier.classify_pages(pages)
        fast_p = sum(1 for p in pages.values() if p.tier == "FAST")
        print(f"  {len(pages)} unique pages, {fast_p} FAST tier")

    if args.mode in ('object', 'both') and args.prof:
        print("\n[Step 3b] Object-level analysis...")
        all_allocs = []
        for pf in args.prof:
            all_allocs.extend(ProfParser.parse(pf, args.min_alloc_size))
        oa = ObjectAnalyzer(pmu)
        oa.load_allocations(all_allocs)
        objects = oa.analyze(records, args.pid)
        TierClassifier.classify_objects(objects)
        print(f"  {len(oa.alloc_ranges)} alloc ranges, {len(objects)} objects matched")

    # 4. Report
    print_report(pages, objects, pmu, total_raw, errors)

    # 5. CSV
    write_csv(pages, objects, args.output)


if __name__ == '__main__':
    main()
