/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/engine/load_worker.h"
#include "scanner/engine/sampling.h"

#include "storehouse/storage_backend.h"

#include <glog/logging.h>

using storehouse::StoreResult;
using storehouse::WriteFile;
using storehouse::RandomReadFile;

namespace scanner {
namespace internal {
namespace {
std::tuple<size_t, size_t>
find_keyframe_indices(i32 start_frame, i32 end_frame,
                      const std::vector<i64> &keyframe_positions) {
  size_t start_keyframe_index = std::numeric_limits<size_t>::max();
  for (size_t i = 1; i < keyframe_positions.size(); ++i) {
    if (keyframe_positions[i] > start_frame) {
      start_keyframe_index = i - 1;
      break;
    }
  }
  assert(start_keyframe_index != std::numeric_limits<size_t>::max());

  size_t end_keyframe_index = 0;
  for (size_t i = start_keyframe_index; i < keyframe_positions.size(); ++i) {
    if (keyframe_positions[i] >= end_frame) {
      end_keyframe_index = i;
      break;
    }
  }
  assert(end_keyframe_index != 0);
  return std::make_tuple(start_keyframe_index, end_keyframe_index);
}

struct VideoIndexEntry {
  i32 width;
  i32 height;
  std::unique_ptr<RandomReadFile> file;
  u64 file_size;
  std::vector<i64> keyframe_positions;
  std::vector<i64> keyframe_byte_offsets;
};

VideoIndexEntry read_video_index(storehouse::StorageBackend *storage,
                                 i32 table_id, i32 column_id, i32 item_id) {
  VideoIndexEntry index_entry;
  VideoMetadata video_meta = read_video_metadata(
      storage, VideoMetadata::descriptor_path(table_id, column_id, item_id));

  // Open the video file for reading
  index_entry.width = video_meta.width();
  index_entry.height = video_meta.height();
  BACKOFF_FAIL(storehouse::make_unique_random_read_file(
      storage, table_item_output_path(table_id, column_id, item_id),
      index_entry.file));
  BACKOFF_FAIL(index_entry.file->get_size(index_entry.file_size));
  index_entry.keyframe_positions = video_meta.keyframe_positions();
  index_entry.keyframe_byte_offsets = video_meta.keyframe_byte_offsets();
  // Place total frames at the end of keyframe positions and total file size
  // at the end of byte offsets to make interval calculation not need to
  // deal with edge cases surrounding those
  index_entry.keyframe_positions.push_back(video_meta.frames());
  index_entry.keyframe_byte_offsets.push_back(index_entry.file_size);

  return index_entry;
}

void read_video_column(Profiler &profiler, VideoIndexEntry &index_entry,
                       const std::vector<i64> &rows, RowList &row_list) {
  RandomReadFile *video_file = index_entry.file.get();
  u64 file_size = index_entry.file_size;
  const std::vector<i64> &keyframe_positions = index_entry.keyframe_positions;
  const std::vector<i64> &keyframe_byte_offsets =
      index_entry.keyframe_byte_offsets;

  // Read the bytes from the file that correspond to the sequences of
  // frames we are interested in decoding. This sequence will contain
  // the bytes starting at the iframe at or preceding the first frame
  // we are interested and will continue up to the bytes before the
  // iframe at or after the last frame we are interested in.
  VideoIntervals intervals =
      slice_into_video_intervals(keyframe_positions, rows);
  size_t num_intervals = intervals.keyframe_index_intervals.size();
  for (size_t i = 0; i < num_intervals; ++i) {
    size_t start_keyframe_index;
    size_t end_keyframe_index;
    std::tie(start_keyframe_index, end_keyframe_index) =
        intervals.keyframe_index_intervals[i];

    u64 start_keyframe_byte_offset =
        static_cast<u64>(keyframe_byte_offsets[start_keyframe_index]);
    u64 end_keyframe_byte_offset =
        static_cast<u64>(keyframe_byte_offsets[end_keyframe_index]);

    i64 start_keyframe = keyframe_positions[start_keyframe_index];
    i64 end_keyframe = keyframe_positions[end_keyframe_index];
    std::vector<i64> all_keyframes;
    for (size_t i = start_keyframe_index; i < end_keyframe_index + 1; ++i) {
      all_keyframes.push_back(keyframe_positions[i]);
    }

    std::vector<i64> all_keyframes_byte_offsets;
    for (size_t i = start_keyframe_index; i < end_keyframe_index + 1; ++i) {
      all_keyframes_byte_offsets.push_back(keyframe_byte_offsets[i] -
                                           start_keyframe_byte_offset);
    }

    size_t buffer_size = end_keyframe_byte_offset - start_keyframe_byte_offset;
    u8 *buffer = new_buffer(CPU_DEVICE, buffer_size);

    auto io_start = now();

    u64 pos = start_keyframe_byte_offset;
    s_read(video_file, buffer, buffer_size, pos);

    profiler.add_interval("io", io_start, now());
    profiler.increment("io_read", static_cast<i64>(buffer_size));

    proto::DecodeArgs decode_args;
    decode_args.set_width(index_entry.width);
    decode_args.set_height(index_entry.height);
    decode_args.set_start_keyframe(keyframe_positions[start_keyframe_index]);
    decode_args.set_end_keyframe(keyframe_positions[end_keyframe_index]);
    for (i64 k : all_keyframes) {
      decode_args.add_keyframes(k);
    }
    for (i64 k : all_keyframes_byte_offsets) {
      decode_args.add_keyframe_byte_offsets(k);
    }
    for (size_t j = 0; j < intervals.valid_frames[i].size(); ++j) {
      decode_args.add_valid_frames(intervals.valid_frames[i][j]);
    }
    decode_args.set_encoded_video(buffer, buffer_size);

    size_t size = decode_args.ByteSize();
    u8 *decode_args_buffer = new_buffer(CPU_DEVICE, size);
    decode_args.SerializeToArray(decode_args_buffer, size);
    INSERT_ROW(row_list, decode_args_buffer, size);

    delete_buffer(CPU_DEVICE, buffer);
  }
}

void read_other_column(storehouse::StorageBackend *storage, i32 table_id,
                       i32 column_id, i32 item_id, i32 item_start, i32 item_end,
                       const std::vector<i64> &rows, RowList &row_list) {
  const std::vector<i64> &valid_offsets = rows;

  std::unique_ptr<RandomReadFile> file;
  StoreResult result;
  BACKOFF_FAIL(make_unique_random_read_file(
      storage, table_item_output_path(table_id, column_id, item_id), file));

  u64 file_size = 0;
  BACKOFF_FAIL(file->get_size(file_size));

  // Read number of rows in file
  u64 pos = 0;
  u64 num_rows = s_read<u64>(file.get(), pos);

  // Read row sizes from work item file header
  std::vector<i64> row_sizes(num_rows);
  s_read(file.get(), reinterpret_cast<u8 *>(row_sizes.data()),
         row_sizes.size() * sizeof(i64), pos);

  // Determine start and end position of rows to read in file
  u64 start_offset = 0;
  for (i64 i = 0; i < item_start; ++i) {
    start_offset += row_sizes[i];
  }
  u64 end_offset = start_offset;
  for (i64 i = item_start; i < item_end; ++i) {
    end_offset += row_sizes[i];
  }
  u64 row_data_size = end_offset - start_offset;
  std::vector<u8> row_data(row_data_size);

  // Read chunk of file corresponding to requested rows
  pos += start_offset;
  s_read(file.get(), row_data.data(), row_data.size(), pos);

  // Extract individual rows and insert into output work entry
  u64 offset = 0;
  size_t valid_idx = 0;
  for (i32 i = item_start; i < item_end; ++i) {
    size_t buffer_size = static_cast<size_t>(row_sizes[i]);
    if (i == valid_offsets[valid_idx]) {
      u8 *buffer = new_buffer(CPU_DEVICE, buffer_size);
      memcpy(buffer, row_data.data() + offset, buffer_size);
      INSERT_ROW(row_list, buffer, buffer_size);
      valid_idx++;
    }
    offset += buffer_size;
  }
  assert(valid_idx == valid_offsets.size());
}
}

void *load_thread(void *arg) {
  LoadThreadArgs &args = *reinterpret_cast<LoadThreadArgs *>(arg);

  auto setup_start = now();

  const i32 work_item_size = args.job_params->work_item_size();

  // Setup a distinct storage backend for each IO thread
  storehouse::StorageBackend *storage =
      storehouse::StorageBackend::make_from_config(args.storage_config);

  // Caching table metadata
  std::map<i32, TableMetadata> table_metadata;

  // To ammortize opening files
  i32 last_table_id = -1;
  std::vector<VideoIndexEntry> index;

  args.profiler.add_interval("setup", setup_start, now());
  while (true) {
    auto idle_start = now();

    std::tuple<IOItem, LoadWorkEntry> entry;
    args.load_work.pop(entry);
    IOItem& io_item = std::get<0>(entry);
    LoadWorkEntry& load_work_entry = std::get<1>(entry);

    if (load_work_entry.io_item_index() == -1) {
      break;
    }

    VLOG(1) << "Load (N/PU: " << args.node_id << "/" << args.id
              << "): processing item " << load_work_entry.io_item_index();

    args.profiler.add_interval("idle", idle_start, now());

    auto work_start = now();

    const auto &samples = load_work_entry.samples();

    if (io_item.table_id() != last_table_id) {
      // Not from the same task so clear cached data
      last_table_id = io_item.table_id();
      index.clear();
    }

    EvalWorkEntry eval_work_entry;
    eval_work_entry.io_item_index = load_work_entry.io_item_index();

    // Aggregate all sample columns so we know the tuple size
    assert(!samples.empty());
    eval_work_entry.warmup_rows = samples.Get(0).warmup_rows_size();

    i32 num_columns = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
      num_columns += samples.Get(i).column_ids_size();
    }
    eval_work_entry.columns.resize(num_columns);

    i32 media_col_idx = 0;
    i32 out_col_idx = 0;
    for (const proto::LoadSample &sample : samples) {
      i32 table_id = sample.table_id();
      auto it = table_metadata.find(table_id);
      if (it == table_metadata.end()) {
        table_metadata[table_id] = read_table_metadata(
            storage, TableMetadata::descriptor_path(table_id));
        it = table_metadata.find(table_id);
      }
      const TableMetadata &table_meta = it->second;

      const google::protobuf::RepeatedField<i64> &sample_warmup_rows =
          sample.warmup_rows();
      const google::protobuf::RepeatedField<i64> &sample_rows = sample.rows();
      std::vector<i64> rows(sample_warmup_rows.begin(),
                            sample_warmup_rows.end());
      rows.insert(rows.end(), sample_rows.begin(), sample_rows.end());
      RowIntervals intervals = slice_into_row_intervals(table_meta, rows);
      size_t num_items = intervals.item_ids.size();
      for (i32 col_id : sample.column_ids()) {
        ColumnType column_type = ColumnType::Other;
        if (table_meta.column_type(col_id) == ColumnType::Video) {
          column_type = ColumnType::Video;
          // video frame column
          for (size_t i = 0; i < num_items; ++i) {
            i32 item_id = intervals.item_ids[i];
            const std::vector<i64> &valid_offsets = intervals.valid_offsets[i];

            // TODO(apoms): cache this so we avoid the IO and recompute for each
            //   request
            VideoIndexEntry entry =
                read_video_index(storage, table_id, col_id, item_id);
            read_video_column(args.profiler, entry, valid_offsets,
                              eval_work_entry.columns[out_col_idx]);
          }
          media_col_idx++;
        } else if (col_id > 0 &&
                   // Convention is that frame info column is immediately
                   // after frame column
                   table_meta.column_type(col_id - 1) == ColumnType::Video) {
          // video meta column
          VideoIndexEntry entry =
              read_video_index(storage, table_id, col_id - 1, 0);
          proto::FrameInfo frame_info;
          frame_info.set_width(entry.width);
          frame_info.set_height(entry.height);

          size_t frame_info_size = frame_info.ByteSize();
          for (size_t i = 0; i < num_items; ++i) {
            size_t total_rows = intervals.valid_offsets[i].size();
            u8 *buffer = new_block_buffer(
                CPU_DEVICE, frame_info_size * total_rows, total_rows);
            for (size_t j = 0; j < intervals.valid_offsets[i].size(); ++j) {
              u8 *b = buffer + frame_info_size * j;
              frame_info.SerializeToArray(b, frame_info_size);
              INSERT_ROW(eval_work_entry.columns[out_col_idx], b,
                         frame_info_size);
            }
          }
        } else {
          // regular column
          for (size_t i = 0; i < num_items; ++i) {
            i32 item_id = intervals.item_ids[i];
            i64 item_start;
            i64 item_end;
            std::tie(item_start, item_end) = intervals.item_intervals[i];
            const std::vector<i64> &valid_offsets = intervals.valid_offsets[i];

            read_other_column(storage, table_id, col_id, item_id, item_start,
                              item_end, valid_offsets,
                              eval_work_entry.columns[out_col_idx]);
          }
        }
        eval_work_entry.column_types.push_back(column_type);
        eval_work_entry.column_handles.push_back(CPU_DEVICE);
        out_col_idx++;
      }
    }

    args.profiler.add_interval("task", work_start, now());

    args.eval_work.push(std::make_tuple(io_item, eval_work_entry));
  }

  VLOG(1) << "Load (N/PU: " << args.node_id << "/" << args.id
            << "): thread finished";

  // Cleanup
  delete storage;

  THREAD_RETURN_SUCCESS();
}
}
}
