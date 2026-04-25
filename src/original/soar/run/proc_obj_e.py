import sys
import csv
import matplotlib
import matplotlib.pyplot as plt
from matplotlib.pyplot import cm
import numpy as np
import os
import pylab
import math
import pandas as pd
import re
from colour import Color
import bisect
from collections import OrderedDict
import multiprocessing
import datetime
from intervaltree import IntervalTree
import gc
import polars as pl

nproc = 1
df_obj_deletes = None

addrmap = {}

obj_names = []
obj_set = set()
obj_creates = []
obj_deletes = []
color_map = {}
interval = 1000000000

cmap = matplotlib.cm.get_cmap('Blues')

directory = "alloc"

events = ["cycles", "CYCLE_ACTIVITY.STALLS_L3_MISS", \
  "OFFCORE_REQUESTS.DEMAND_DATA_RD", \
  "OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD"]

def get_time():
  current_time = datetime.datetime.now()
  print(current_time.strftime("%H:%M:%S"))

def time_log(msg):
  current_time = datetime.datetime.now()
  print(current_time.strftime("%H:%M:%S"), msg)

def check_saddr(obj_name):
  addr = obj_name[2:]
  addr_d = int(addr, 16)
  # print(addr, addr_d)
  if addr_d < 5242880:
    return 1
  return 0

def process_file(filename):
  data = []
  time_start = 0
  line_cnt = 0
  f =  open(filename, 'r')
  lines = f.readlines()
  created = 0
  deleted = 0
  # Process the file content here
  for line in lines:
    # print(line.strip())
    line1 = line.strip()
    processed_line = line1.split(" ")
    if len(processed_line) == 6: # line_addr time size addr type
      curr_time = int(processed_line[2])

      time = curr_time
      size = int(processed_line[3])
      type = int(processed_line[5])
      # Check obj size
      if type !=2 and size < 4096:
        continue
      addr = int(processed_line[4], 16)
      obj_name = processed_line[1]
      if "[" in obj_name and ']' in obj_name:
        obj_name = obj_name[1:-1]
      else:
        print("WARNING", obj_name)
      ret = check_saddr(obj_name)
      if ret == 0:
        continue
      # check callchain
      if (len(obj_name) < 1):
        if type == 2:
          data.append([time, size, addr, type, ""])
          deleted += 1
      else:
        data.append([time, size, addr, type, obj_name])
        created += 1
    line_cnt += 1
  f.close()
  # if len(data) > 0:
  #   print(data[0][0], data[-1][0])
  # print("created", created)
  return data

def get_alloc_data(df, df_addr):
  # [time, size, addr, type, obj_name]
  max_time = df_addr.select(pl.col("time").max()).item()
  print("max_time", max_time)
  allocs = df.filter(pl.col("type") != 2)
  time_log("AFTER allocs")

  deallocs = df.filter(pl.col("type") == 2).select([
    pl.col("time").alias("dealloc_time"),
    pl.col("addr")
  ])
  time_log("AFTER deallocs")

  joined = allocs.join_asof(
    deallocs,
    left_on="time",
    right_on="dealloc_time",
    by="addr",
    strategy="forward"
  )

  print(joined)

  time_log("AFTER joined")

  result = joined.with_columns([
    pl.col("time").alias("alloc_time"),
    pl.col("addr").alias("start_addr"),
    pl.col("dealloc_time").fill_null(max_time)
  ]).select([
    "alloc_time", "dealloc_time", "start_addr", "size", "obj_name"
  ])
  return result

def merge_intervals(intervals):
  sorted_intervals = sorted(intervals, key=lambda x: x[0])
  merged = []
  for interval in sorted_intervals:
    if not merged or merged[-1][1] < interval[0]:
      merged.append(interval)
    else:
      merged[-1][1] = max(merged[-1][1], interval[1])
  return merged

def find_idx_in_range(arr, lower_bound, upper_bound):
  start_index = bisect.bisect_left(arr, lower_bound)
  end_index = bisect.bisect_right(arr, upper_bound)
  return start_index, end_index

def check_obj_access(accesses_perf_with_addr_range, obj):
  # interval = 1000000000
  accesses = []
  for t, item in enumerate(accesses_perf_with_addr_range):
    cnt = 0
    if obj in item.keys():
      cnt = item[obj][0]
    accesses.append([t, cnt])
  return accesses

def check_obj_accesses_perf_with_addr_range_t(data):
  global df_obj_deletes
  all_time, all_addr, new_ts = data
  accesses_perf = []
  if len(new_ts) < 1:
    return accesses_perf
  for i in range(len(new_ts)):
    accesses_perf.append({})
  for row in df_obj_deletes.iter_rows():
    start_time, end_time, addr, size, obj_name = row
    low_index, high_index = find_idx_in_range(new_ts, start_time, end_time)
    for idx in range(low_index, high_index):
      start_t = new_ts[idx]
      if idx+1 == high_index:
        end_t = new_ts[idx]
      else:
        end_t = new_ts[idx+1]
      start_index, end_index = find_idx_in_range(all_time, start_t, end_t)
      cnt = 0
      for j in range(start_index, end_index):
        if all_addr[j] >= addr and all_addr[j] <= addr + size:
          cnt += 1
      content = accesses_perf[idx]
      if obj_name in content.keys():
        accesses_perf[idx][obj_name][0] += cnt
        accesses_perf[idx][obj_name][1].append([addr, addr + size])
      else:
        accesses_perf[idx][obj_name] = [cnt, [[addr, addr + size]]]
  for i in range(len(new_ts)):
    for obj in obj_set:
      if obj in accesses_perf[i].keys():
        merged = merge_intervals(accesses_perf[i][obj][1])
        accesses_perf[i][obj][1] = merged
  return accesses_perf

def check_obj_accesses_perf_with_addr_range_p(all_time, all_addr, new_ts):
  accesses_perf = []

  data_list = []
  start_idx = 0
  length = len(new_ts)

  for i in range(nproc):
    start_idx = i * int(length/nproc)
    end_idx = (i+1) * int(length/nproc)
    if i == nproc - 1:
      end_idx = length
    # print(start_idx, end_idx)
    new_ts_i = new_ts[start_idx:end_idx]
    data_list.append([all_time, all_addr, new_ts_i])

  print("running...")
  with multiprocessing.Pool(processes=nproc) as pool:
    results = pool.map(check_obj_accesses_perf_with_addr_range_t, data_list)

  print("merge")
  del data_list
  gc.collect()
  print(len(results))
  for i in range(len(results)):
    accesses_perf += results[i]
  return accesses_perf

def check_obj_accesses_perf_with_addr_range(all_time, all_addr, new_ts):
  accesses_perf = []
  for i in range(len(new_ts)):
    accesses_perf.append({})
  for _, obj_l in enumerate(obj_deletes):
    start_time = obj_l[0]
    end_time = obj_l[1]
    obj_name = obj_l[2]
    addr = obj_l[3]
    size = obj_l[4]
    low_index, high_index = find_idx_in_range(new_ts, start_time, end_time)
    for idx in range(low_index, high_index-1):
      start_t = new_ts[idx]
      end_t = new_ts[idx+1]
      start_index, end_index = find_idx_in_range(all_time, start_t, end_t)
      cnt = 0
      for j in range(start_index, end_index):
        if all_addr[j] >= addr and all_addr[j] <= addr + size:
          cnt += 1
      content = accesses_perf[idx]
      if obj_name in content.keys():
        accesses_perf[idx][obj_name][0] += cnt
        accesses_perf[idx][obj_name][1].append([addr, addr + size])
      else:
        accesses_perf[idx][obj_name] = [cnt, [[addr, addr + size]]]
    merged = merge_intervals(accesses_perf[idx][obj_name][1])
    accesses_perf[idx][obj_name][1] = merged
  return accesses_perf

def read_file(file):
  addr_1, addr_2, addr_3, addr_4 = [], [], [], []
  time_1, time_2, time_3, time_4 = [], [], [], []
  line_count_1, line_count_2, line_count_3, line_count_4 = 0, 0, 0, 0
  f = open(file, 'r')
  lines = f.readlines()
  for index, line in enumerate(lines):
    if "pebs:pebs" in line:
      arr = re.split(' |,|\n', line)
      arr = [x for x in arr if len(x) > 0]
      # print(arr)
      address = int(arr[8], 16)
      time = int(arr[10], 16)
      # TODO
      line_count_1 += 1
      addr_1.append(address)
      time_1.append(time)
  print(f'Processed {line_count_1} lines.')
  return [[time_1, addr_1, line_count_1]]

def plot_acc(acc, obj, output_path):
  x = [a[0] for a in acc]
  y = [a[1] for a in acc]
  plt.plot(x, y)
  plt.savefig(output_path + '/' + "acc_" + obj + '.png')
  plt.clf()

def plot_perf_data(x, y, output_path, filename, xlabel, ylabel):
  min_len = min(len(x), len(y))
  plt.plot(x[:min_len], y[:min_len])
  plt.xlabel(xlabel)
  plt.ylabel(ylabel)
  plt.savefig(output_path + '/' + "perf_" + filename + '.png')
  plt.clf()

def plot_acc_all(accs, objs, output_path):
  for i, acc in enumerate(accs):
    x = [a[0] for a in acc]
    y = [a[1] for a in acc]
    plt.plot(x, y, label=objs[i], color=color_map[objs[i]])
  plt.legend(frameon=False)
  plt.savefig(output_path + '/' + "acc_all" + '.png')
  plt.clf()

def plot_acc_all_aol(accs, objs, plots_path, a_lat):
  fig, ax1 = plt.subplots()
  lines = []
  for i, acc in enumerate(accs):
    x = [a[0] for a in acc]
    y = [a[1] for a in acc]
    line1 = ax1.plot(x, y, label=objs[i], color=color_map[objs[i]])[0]
    lines.append(line1)
  ax2 = ax1.twinx()
  ax2.plot(list(range(len(a_lat))), a_lat)[0]
  labels = [line.get_label() for line in lines]
  plt.legend(lines, labels, frameon=False)
  plt.savefig(plots_path + '/' + "acc_all_aol" + '.png')
  plt.clf()

def rank_dict_values(d):
  sorted_items = sorted(d.items(), key=lambda x: x[1], reverse=True)
  ranks = {k: i + 1 for i, (k, v) in enumerate(sorted_items)}
  return ranks

def rank_objs_r(accesses_perf_with_addr_range, new_ts, est_dram_sd, a_lat, is_mlp):
  obj_scores = {}
  obj_time_span = {}
  global obj_set
  for o in obj_set:
    print(o)
    obj_scores[o] = 0
    obj_time_span[o] = 0
  for i in range(len(accesses_perf_with_addr_range)):
    content = accesses_perf_with_addr_range[i]
    if len(content) > 0:
      all_acc = 0
      for obj1 in content.keys():
        all_acc += content[obj1][0]
      has_mlp = 0
      for obj2 in content.keys():
        if all_acc > 0 and a_lat[i] <= 80:
          has_mlp = 1
      for obj in content.keys():
        if all_acc == 0:
          obj_scores[obj] += 0
        else:
          if is_mlp:
            factor = 1
            min_ratio = 0.03
            max_ratio = 0.7
            if a_lat[i] <= 80 and a_lat[i] > 60:
              factor = 2
              min_ratio = 0.4
              max_ratio = 0.6
            elif a_lat[i] <= 60 and a_lat[i] > 45:
              factor = 4
            elif a_lat[i] > 40:
              factor = 8
            else:
              factor = 12
            if has_mlp > 0:
              if content[obj][0]/all_acc >= max_ratio:
                obj_scores[obj] += ((content[obj][0]/all_acc) * est_dram_sd[i]) / factor
              elif content[obj][0]/all_acc >= min_ratio:
                obj_scores[obj] += ((content[obj][0]/all_acc) * est_dram_sd[i])
              else:
                obj_scores[obj] += ((content[obj][0]/all_acc) * est_dram_sd[i]) * factor
            else:
              obj_scores[obj] += (content[obj][0]/all_acc) * est_dram_sd[i]
          else:
            obj_scores[obj] += content[obj][0]
        obj_time_span[obj] += 1
  return obj_scores, obj_time_span

def get_max_range_for_objs(accesses_perf_with_addr_range, new_ts, est_dram_sd):
  obj_max_range = {}
  obj_time_span = {}
  for o in obj_set:
    obj_max_range[o] = 0
    obj_time_span[o] = 0
  for i in range(len(accesses_perf_with_addr_range)):
    content = accesses_perf_with_addr_range[i]
    if len(content) > 0:
      all_acc = 0
      all_addr_range = 0
      for obj in content.keys():
        obj_max_range[obj] = max(obj_max_range[obj], \
          float(sum(end - start for [start, end] in content[obj][1])))
  return obj_max_range

def create_csv(time, addr, csvname):
  df = pl.DataFrame({
    "time": pl.Series("time", time, dtype=pl.UInt64),
    "addr": pl.Series("addr", addr, dtype=pl.UInt64)
  })
  df = df.sort("time")
  df.write_csv(csvname)

def create_data_csv(obj_data_all, csvname):
  df = pl.DataFrame({
    "time": pl.Series("time", [x[0] for x in obj_data_all], dtype=pl.UInt64),
    "size": [x[1] for x in obj_data_all],
    "addr": pl.Series("time", [x[2] for x in obj_data_all], dtype=pl.UInt64),
    "type": [x[3] for x in obj_data_all],
    "obj_name": [x[4] for x in obj_data_all]
  })
  df = df.unique()
  df = df.sort("time")
  df.write_csv(csvname)
  return df

def create_obj_rank_csv(input, csvname):
  data = {"obj_name": input.keys(), \
    "value": input.values()}
  df = pd.DataFrame(data)
  df.to_csv(csvname)

def create_obj_stat_csv(data, csvname):
  # objs, scores, max_range, s_per_range, time_span
  objs, scores, max_range, s_per_range, time_span = data
  data = {"obj_name": objs, "scores": scores, \
    "max_range": max_range, "score_per_range": s_per_range, \
    "time_span": time_span}
  df = pd.DataFrame(data)
  df.to_csv(csvname)

def process_perf(fname):
  res = OrderedDict()
  for event in events:
    res[event] = []
  with open(fname) as csv_file:
    # Process the file content here
    csv_header = csv.reader(csv_file, delimiter=' ')
    line_count = 0
    for row in csv_header:
      line_count += 1
      processed_row = [item for item in row if item]
      for event in events:
        if event in processed_row:
          value = float(processed_row[1].replace(',', ''))
          res[event].append(value)
  return res

def process_all(obj_scores, obj_time_span, ranked_obj_scores, obj_max_range):
  objs = ranked_obj_scores.keys()
  scores = []
  max_range = []
  s_per_range = []
  time = []
  for obj in objs:
    scores.append(obj_scores[obj])
    max_range.append(obj_max_range[obj])
    if obj_max_range[obj] == 0:
      s_per_range.append(0)
    else:
      s_per_range.append(obj_scores[obj]/obj_max_range[obj])
    time.append(obj_time_span[obj])
  return objs, scores, max_range, s_per_range, time

def get_colors(n):
  res = []
  for i in range(n):
    color = cmap((i+1)/(n+1))
    res.append(color)
  return res

def rank_obj_with_s(objs, s_per_range):
  objmap = {}
  for i, o in enumerate(objs):
    objmap[o] = s_per_range[i]
  ranked = rank_dict_values(objmap)
  colors = get_colors(len(objs))
  reversed_colors = colors[::-1]
  for j, k in enumerate(ranked.keys()):
    color_map[k] = reversed_colors[j]

  ranked_objs = list(ranked.keys())
  ranks = list(ranked.values())
  ranked_data = {"rank": ranks, "obj": ranked_objs}
  df = pd.DataFrame(ranked_data)
  df = df.sort_values("rank")
  df.to_csv("obj_rank.csv")

def aol_to_csv(new_ts, a_lat):
  data = {"time": new_ts, "lat": a_lat}
  df = pd.DataFrame(data)
  df.to_csv("obj_aol.csv")

def accs_to_csv(accs, objs):
  ob = []
  ts = []
  ac = []
  for i, acc in enumerate(accs):
    x = [a[0] for a in acc]
    y = [a[1] for a in acc]
    o = [objs[i] for a in acc]
    ts += x
    ac += y
    ob += o
  data = {"obj": ob, "time": ts, "acc": ac}
  df = pd.DataFrame(data)
  df.to_csv("obj_acc.csv")

def main():
  get_time()
  if len(sys.argv) < 2:
    print("Please provide directory name as the argument.")
    exit()
  directory = sys.argv[1]

  plots_path = "plots"
  isExist = os.path.exists(plots_path)
  if not isExist:
    os.makedirs(plots_path)

  hasfile = os.path.isfile("addr.csv")
  if not hasfile:
    fname = "pebs.txt"
    data = read_file(fname)
    all_time = data[0][0]
    all_addr = data[0][1]
    create_csv(all_time, all_addr, "addr.csv")

  df_addr = pl.read_csv("addr.csv", dtypes={"time": pl.UInt64, "addr": pl.UInt64})

  df = None
  hasfile = os.path.isfile("obj_data.csv")
  if not hasfile:
    obj_data_all = []
    files = os.listdir(directory)
    for file in files:
      obj_filename = os.path.join(directory, file)
      obj_data = process_file(obj_filename)
      obj_data_all += obj_data
    print(len(obj_data_all))
    df = create_data_csv(obj_data_all, "obj_data.csv")
  else:
    df = pl.read_csv("obj_data.csv", dtypes={"time": pl.UInt64, "addr": pl.UInt64})
  assert df is not None

  global df_obj_deletes
  time_log("BEFORE get_alloc_data")
  df_obj_deletes = get_alloc_data(df, df_addr)
  df_obj_deletes.write_csv("obj_deletes.csv")
  time_log("AFTER get_alloc_data")

  global obj_set
  obj_set = set(df_obj_deletes["obj_name"])
  print(obj_set)

  colors = ["maroon", "red", "orange", "yellow", "yellowgreen", \
    "lawngreen", "cyan", "royalblue", "navy", "slateblue", \
    "mediumpurple", "violet", "mediumvioletred", "deeppink", \
    "pink"]
  if len(colors) < len(obj_set):
    color = iter(cm.rainbow(np.linspace(0, 1, len(obj_set))))
    colors = []
    for i in range(len(obj_set)):
      colors.append(next(color))
  for idx, obj in enumerate(obj_set):
    color_map[obj] = colors[idx]

  start_time = df_obj_deletes.select(pl.col("alloc_time").min()).item()
  end_time = df_obj_deletes.select(pl.col("dealloc_time").max()).item()

  # [time, size, addr, type, obj_name]
  perf_data = process_perf("perf.data")

  perf_interval = int((end_time - start_time) / len(perf_data[events[0]]))
  new_ts = [t for t in range(int(start_time), int(end_time), perf_interval)]
  new_ts = new_ts[:-1]
  demand_data_rd_idx = events.index("OFFCORE_REQUESTS.DEMAND_DATA_RD")
  cyc_demand_data_rd_idx = events.index("OFFCORE_REQUESTS_OUTSTANDING.CYCLES_WITH_DEMAND_DATA_RD")
  l3_stall_idx = events.index("CYCLE_ACTIVITY.STALLS_L3_MISS")
  cyc_idx = events.index("cycles")
  demand_data_rd = np.asarray(perf_data[events[demand_data_rd_idx]])
  cyc_demand_data_rd = np.asarray(perf_data[events[cyc_demand_data_rd_idx]])
  l3_stall = np.asarray(perf_data[events[l3_stall_idx]])
  cyc = np.asarray(perf_data[events[cyc_idx]])

  l3_stall_per_cyc = l3_stall/cyc
  a_lat = cyc_demand_data_rd/demand_data_rd
  est_dram_sd_2 = [l3_stall_per_cyc[i]/(24.67/a_lat[i]+0.87) for i in range(len(a_lat))]

  # PLOT
  plot_perf_data(new_ts, a_lat, plots_path, \
    "a_lat", "time", "Amortized Offcore Latency")
  aol_to_csv(new_ts, a_lat)
  time_log("AFTER plot_perf_data")

  all_time, all_addr = df_addr["time"], df_addr["addr"]

  print("BEFORE check_obj_accesses_perf")
  accesses_perf_with_addr_range = check_obj_accesses_perf_with_addr_range_p(all_time, all_addr, new_ts)
  time_log("Returned accesses_perf_with_addr_range")
  print("AFTER check_obj_accesses_perf")

  accs = []
  objs = []
  for obj in obj_set:
    acc = check_obj_access(accesses_perf_with_addr_range, obj)
    accs.append(acc)
    objs.append(obj)
    plot_acc(acc, obj, plots_path)
  plot_acc_all(accs, objs, plots_path)
  plot_acc_all_aol(accs, objs, plots_path, a_lat)

  print("-----------------------------")
  obj_scores, obj_time_span = rank_objs_r(accesses_perf_with_addr_range, new_ts, est_dram_sd_2, a_lat, True)
  print(obj_scores)
  create_obj_rank_csv(obj_scores, "obj_scores.csv")
  print(obj_time_span)

  ranked_obj_scores = rank_dict_values(obj_scores)
  print(ranked_obj_scores)
  create_obj_rank_csv(ranked_obj_scores, "ranked_obj_scores.csv")

  print("-----------------------------")
  obj_max_range = get_max_range_for_objs(accesses_perf_with_addr_range, new_ts, est_dram_sd_2)
  print(obj_max_range)
  create_obj_rank_csv(obj_max_range, "obj_max_range.csv")

  objs, scores, max_range, s_per_range, time_span = process_all(obj_scores, obj_time_span, ranked_obj_scores, obj_max_range)
  create_obj_stat_csv([objs, scores, max_range, s_per_range, time_span], "obj_stat.csv")

  obj_scores, _ = rank_objs_r(accesses_perf_with_addr_range, new_ts, est_dram_sd_2, a_lat, False)
  create_obj_rank_csv(obj_scores, "obj_scores_n.csv")

  hasfile = os.path.isfile("obj_rank.csv")
  if not hasfile:
    rank_obj_with_s(objs, s_per_range)
  else:
    df = pd.read_csv("obj_rank.csv")
    objs = df["obj"].to_list()
    colors = get_colors(len(objs))
    reversed_colors = colors[::-1]
    for j, k in enumerate(objs):
      color_map[k] = reversed_colors[j]

  time_log("Ranking FINISHED")

  print("-----------------------------")

  accs = []
  objs = []
  for obj in obj_set:
    acc = check_obj_access(accesses_perf_with_addr_range, obj)
    accs.append(acc)
    objs.append(obj)
  accs_to_csv(accs, objs)

if __name__ == "__main__":
  main()
