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

#include "scanner/engine/runtime.h"
#include "scanner/engine/ingest.h"
#include "scanner/engine/sampler.h"
#include "scanner/util/progress_bar.h"
#include <grpc/support/log.h>

#include <mutex>
namespace scanner {
namespace internal {
namespace {
void validate_task_set(DatabaseMetadata &meta, const proto::TaskSet &task_set,
                       Result *result) {
  auto &tasks = task_set.tasks();
  // Validate tasks
  std::set<std::string> task_output_table_names;
  for (auto &task : task_set.tasks()) {
    if (task.output_table_name() == "") {
      LOG(WARNING) << "Task specified with empty output table name. Output "
                      "tables can not have empty names";
      result->set_success(false);
    }
    if (meta.has_table(task.output_table_name())) {
      LOG(WARNING) << "Task specified with duplicate output table name. "
                   << "A table with name " << task.output_table_name() << " "
                   << "already exists.";
      result->set_success(false);
    }
    if (task_output_table_names.count(task.output_table_name()) > 0) {
      LOG(WARNING) << "Mulitple tasks specified with output table name "
                   << task.output_table_name()
                   << ". Table names must be unique.";
      result->set_success(false);
    }
    task_output_table_names.insert(task.output_table_name());
    if (task.samples().size() == 0) {
      LOG(WARNING) << "Task " << task.output_table_name() << " did not "
                   << "specify any tables to sample from. Tasks must sample "
                   << "from at least one table.";
      result->set_success(false);
    } else {
      for (auto &sample : task.samples()) {
        if (!meta.has_table(sample.table_name())) {
          LOG(WARNING) << "Task " << task.output_table_name() << " tried to "
                       << "sample from non-existent table "
                       << sample.table_name()
                       << ". TableSample must sample from existing table.";
          result->set_success(false);
        }
        // TODO(apoms): validate sampler functions
        if (sample.column_names().size() == 0) {
          LOG(WARNING) << "Task" << task.output_table_name() << " tried to "
                       << "sample zero columns from table "
                       << sample.table_name()
                       << ". TableSample must sample at least one column";
          result->set_success(false);
        }
      }
    }
  }
  // Validate ops
  {
    OpRegistry *op_registry = get_op_registry();
    KernelRegistry *kernel_registry = get_kernel_registry();

    i32 op_idx = 0;
    std::vector<std::string> op_names;
    std::vector<std::vector<std::string>> op_outputs;
    for (auto &op : task_set.ops()) {
      op_names.push_back(op.name());
      if (op_idx == 0) {
        if (op.name() != "InputTable") {
          RESULT_ERROR(result, "First Op is %s but must be Op InputTable",
                       op.name().c_str());
          break;
        }
        op_outputs.emplace_back();
        for (auto &input : op.inputs()) {
          for (auto &col : input.columns()) {
            op_outputs.back().push_back(col);
          }
        }
        op_idx++;
        continue;
      }
      if (op.name() != "OutputTable") {
        op_outputs.emplace_back();
        if (!op_registry->has_op(op.name())) {
          RESULT_ERROR(result, "Op %s is not registered.", op.name().c_str());
        } else {
          op_outputs.back() =
              op_registry->get_op_info(op.name())->output_columns();
        }
        if (!kernel_registry->has_kernel(op.name(), op.device_type())) {
          RESULT_ERROR(result,
                       "Op %s at index %d requested kernel with device type "
                       "%s but no such kernel exists.",
                       op.name().c_str(), op_idx,
                       (op.device_type() == DeviceType::CPU ? "CPU" : "GPU"));
        }
      }
      for (auto &input : op.inputs()) {
        if (input.op_index() >= op_idx) {
          RESULT_ERROR(result,
                       "Op %s at index %d referenced input index %d."
                       "Ops must be specified in topo sort order.",
                       op.name().c_str(), op_idx, input.op_index());
        } else {
          std::string &input_op_name = op_names.at(input.op_index());
          std::vector<std::string> &inputs = op_outputs.at(input.op_index());
          for (auto &col : input.columns()) {
            bool found = false;
            for (auto &out_col : inputs) {
              if (col == out_col) {
                found = true;
                break;
              }
            }
            if (!found) {
              RESULT_ERROR(result,
                           "Op %s at index %d requested column %s from input "
                           "Op %s at index %d but that Op does not have the "
                           "requsted column.",
                           op.name().c_str(), op_idx, col.c_str(),
                           input_op_name.c_str(), input.op_index());
            }
          }
        }
      }
      op_idx++;
    }
    if (op_names.size() < 3) {
      RESULT_ERROR(result,
                   "Task set must specify at least three Ops: "
                   "an InputTable Op, any other Op, and an OutputTable Op. "
                   "However, only %lu Ops were specified.",
                   op_names.size());
    } else {
      if (op_names.front() != "InputTable") {
        RESULT_ERROR(result, "First Op is %s but must be InputTable",
                     op_names.front().c_str());
      }
      if (op_names.back() != "OutputTable") {
        RESULT_ERROR(result, "Last Op is %s but must be OutputTable",
                     op_names.back().c_str());
      }
    }
  }
}

Result
get_task_end_rows(const std::map<std::string, TableMetadata> &table_metas,
                  const proto::Task &task, std::vector<i64> &rows) {
  Result result;
  result.set_success(true);

  TaskSampler sampler(table_metas, task);
  result = sampler.validate();
  if (!result.success()) {
    return result;
  }
  i64 num_samples = sampler.total_samples();
  for (i64 i = 0; i < num_samples; ++i) {
    proto::NewWork new_work;
    result = sampler.next_work(new_work);
    if (!result.success()) {
      rows.clear();
      return result;
    }
    rows.push_back(new_work.io_item().end_row());
  }
  return result;
}
}

class MasterImpl final : public proto::Master::Service {
public:
  MasterImpl(DatabaseParameters &params) : db_params_(params) {
    storage_ =
        storehouse::StorageBackend::make_from_config(db_params_.storage_config);
    set_database_path(params.db_path);
  }

  ~MasterImpl() { delete storage_; }

  grpc::Status RegisterWorker(grpc::ServerContext *context,
                              const proto::WorkerInfo *worker_info,
                              proto::Registration *registration) {
    set_database_path(db_params_.db_path);

    workers_.push_back(proto::Worker::NewStub(grpc::CreateChannel(
        worker_info->address(), grpc::InsecureChannelCredentials())));
    registration->set_node_id(workers_.size() - 1);
    addresses_.push_back(worker_info->address());

    return grpc::Status::OK;
  }

  grpc::Status IngestVideos(grpc::ServerContext *context,
                            const proto::IngestParameters *params,
                            proto::IngestResult *result) {
    std::vector<FailedVideo> failed_videos;
    result->mutable_result()->CopyFrom(
        ingest_videos(db_params_.storage_config, db_params_.db_path,
                      std::vector<std::string>(params->table_names().begin(),
                                               params->table_names().end()),
                      std::vector<std::string>(params->video_paths().begin(),
                                               params->video_paths().end()),
                      failed_videos));
    for (auto& failed : failed_videos) {
      result->add_failed_paths(failed.path);
      result->add_failed_messages(failed.message);
    }
    return grpc::Status::OK;
  }

  grpc::Status NextWork(grpc::ServerContext *context,
                        const proto::NodeInfo *node_info,
                        proto::NewWork *new_work) {
    std::unique_lock<std::mutex> lk(work_mutex_);
    if (samples_left_ <= 0) {
      if (next_task_ < num_tasks_ && task_result_.success()) {
        // More tasks left
        task_sampler_.reset(new TaskSampler(
            table_metas_, job_params_.task_set().tasks(next_task_)));
        task_result_ = task_sampler_->validate();
        if (task_result_.success()) {
          samples_left_ = task_sampler_->total_samples();
          next_task_++;
          VLOG(1) << "Tasks left: " << num_tasks_ - next_task_;
        }
      } else {
        // No more tasks left
        new_work->mutable_io_item()->set_item_id(-1);
        return grpc::Status::OK;
      }
    }
    if (!task_result_.success()) {
      new_work->mutable_io_item()->set_item_id(-1);
      return grpc::Status::OK;
    }

    assert(samples_left_ > 0);
    task_result_ = task_sampler_->next_work(*new_work);
    if (!task_result_.success()) {
      new_work->mutable_io_item()->set_item_id(-1);
      return grpc::Status::OK;
    }

    samples_left_--;
    total_samples_used_++;
    bar_->Progressed(total_samples_used_);
    return grpc::Status::OK;
  }

  grpc::Status NewJob(grpc::ServerContext *context,
                      const proto::JobParameters *job_params,
                      proto::Result *job_result) {
    job_result->set_success(true);
    set_database_path(db_params_.db_path);

    job_params_.CopyFrom(*job_params);

    const i32 io_item_size = job_params->io_item_size();
    const i32 work_item_size = job_params->work_item_size();

    i32 warmup_size = 0;
    i32 total_rows = 0;

    proto::JobDescriptor job_descriptor;
    job_descriptor.set_io_item_size(io_item_size);
    job_descriptor.set_work_item_size(work_item_size);
    job_descriptor.set_num_nodes(workers_.size());

    // Get output columns from last output op
    auto &ops = job_params->task_set().ops();
    // OpRegistry* op_registry = get_op_registry();
    // OpInfo* output_op = op_registry->get_op_info(
    //   ops.Get(ops.size()-1).name());
    // const std::vector<std::string>& output_columns =
    //   output_op->output_columns();
    auto &last_op = ops.Get(ops.size() - 1);
    assert(last_op.name() == "OutputTable");
    std::vector<std::string> output_columns;
    for (const auto &eval_input : last_op.inputs()) {
      for (const std::string &name : eval_input.columns()) {
        output_columns.push_back(name);
      }
    }
    for (size_t i = 0; i < output_columns.size(); ++i) {
      auto &col_name = output_columns[i];
      Column *col = job_descriptor.add_columns();
      col->set_id(i);
      col->set_name(col_name);
      col->set_type(ColumnType::Other);
    }

    DatabaseMetadata meta =
        read_database_metadata(storage_, DatabaseMetadata::descriptor_path());

    auto &tasks = job_params->task_set().tasks();
    job_descriptor.mutable_tasks()->CopyFrom(tasks);

    validate_task_set(meta, job_params->task_set(), job_result);
    if (!job_result->success()) {
      // No database changes made at this point, so just return
      return grpc::Status::OK;
    }

    // Add job name into database metadata so we can look up what jobs have
    // been ran
    i32 job_id = meta.add_job(job_params->job_name());
    job_descriptor.set_id(job_id);
    job_descriptor.set_name(job_params->job_name());

    // Read all table metadata
    for (const std::string &table_name : meta.table_names()) {
      std::string table_path =
          TableMetadata::descriptor_path(meta.get_table_id(table_name));
      table_metas_[table_name] = read_table_metadata(storage_, table_path);
    }

    total_samples_used_ = 0;
    total_samples_ = 0;
    for (auto &task : job_params->task_set().tasks()) {
      i32 table_id = meta.add_table(task.output_table_name());
      proto::TableDescriptor table_desc;
      table_desc.set_id(table_id);
      table_desc.set_name(task.output_table_name());
      table_desc.set_timestamp(std::chrono::duration_cast<std::chrono::seconds>(
                                   now().time_since_epoch())
                                   .count());
      // Set columns equal to the last op's output columns
      for (size_t i = 0; i < output_columns.size(); ++i) {
        Column *col = table_desc.add_columns();
        col->set_id(i);
        col->set_name(output_columns[i]);
        col->set_type(ColumnType::Other);
      }
      table_metas_[task.output_table_name()] = TableMetadata(table_desc);
      std::vector<i64> end_rows;
      Result result = get_task_end_rows(table_metas_, task, end_rows);
      if (!result.success()) {
        *job_result = result;
        break;
      }
      total_samples_ += end_rows.size();
      for (i64 r : end_rows) {
        table_desc.add_end_rows(r);
      }
      table_desc.set_job_id(job_id);

      write_table_metadata(storage_, TableMetadata(table_desc));
      table_metas_[task.output_table_name()] = TableMetadata(table_desc);
    }
    if (!job_result->success()) {
      // No database changes made at this point, so just return
      return grpc::Status::OK;
    }

    // Write out database metadata so that workers can read it
    write_job_metadata(storage_, JobMetadata(job_descriptor));

    // Setup initial task sampler
    task_result_.set_success(true);
    samples_left_ = 0;
    next_task_ = 0;
    num_tasks_ = job_params->task_set().tasks_size();

    write_database_metadata(storage_, meta);

    VLOG(1) << "Total tasks: " << num_tasks_;

    grpc::CompletionQueue cq;
    std::vector<grpc::ClientContext> client_contexts(workers_.size());
    std::vector<grpc::Status> statuses(workers_.size());
    std::vector<proto::Result> replies(workers_.size());
    std::vector<std::unique_ptr<grpc::ClientAsyncResponseReader<proto::Result>>>
        rpcs;

    bar_ = new ProgressBar(total_samples_, "");

    std::map<std::string, i32> local_ids;
    std::map<std::string, i32> local_totals;
    for (std::string &address : addresses_) {
      if (local_totals.count(address) == 0) {
        local_totals[address] = 0;
      }
      local_totals[address] += 1;
    }

    proto::JobParameters w_job_params;
    w_job_params.CopyFrom(*job_params);
    for (size_t i = 0; i < workers_.size(); ++i) {
      auto &worker = workers_[i];
      std::string &address = addresses_[i];
      if (local_ids.count(address) == 0) {
        local_ids[address] = 0;
      }
      w_job_params.set_local_id(local_ids[address]);
      w_job_params.set_local_total(local_totals[address]);
      local_ids[address] += 1;
      rpcs.emplace_back(
          worker->AsyncNewJob(&client_contexts[i], w_job_params, &cq));
      rpcs[i]->Finish(&replies[i], &statuses[i], (void *)i);
    }

    for (size_t i = 0; i < workers_.size(); ++i) {
      void *got_tag;
      bool ok = false;
      GPR_ASSERT(cq.Next(&got_tag, &ok));
      GPR_ASSERT((i64)got_tag < workers_.size());
      assert(ok);

      if (!replies[i].success()) {
        LOG(WARNING) << "Worker returned error: " << replies[i].msg();
        job_result->set_success(false);
        job_result->set_msg(replies[i].msg());
        next_task_ = num_tasks_;
      }
    }

    if (!job_result->success()) {
      // TODO(apoms): We wrote the db meta with the tables so we should clear
      // them out here since the job failed.
    }
    if (!task_result_.success()) {
      job_result->CopyFrom(task_result_);
    } else {
      assert(next_task_ == num_tasks_);
      bar_->Progressed(total_samples_);
    }

    return grpc::Status::OK;
  }

  grpc::Status Ping(grpc::ServerContext *context, const proto::Empty *empty1,
                    proto::Empty *empty2) {
    return grpc::Status::OK;
  }

  grpc::Status LoadOp(grpc::ServerContext *context,
                      const proto::OpInfo *op_info, Result *result) {
    const std::string &so_path = op_info->so_path();
    {
      std::ifstream infile(so_path);
      if (!infile.good()) {
        RESULT_ERROR(result, "Op library was not found: %s", so_path.c_str());
        return grpc::Status::OK;
      }
    }

    void *handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      RESULT_ERROR(result, "Failed to load op library: %s", dlerror());
      return grpc::Status::OK;
    }

    for (auto &worker : workers_) {
      grpc::ClientContext ctx;
      proto::Empty empty;
      worker->LoadOp(&ctx, *op_info, &empty);
    }

    result->set_success(true);
    return grpc::Status::OK;
  }

private:
  std::vector<std::unique_ptr<proto::Worker::Stub>> workers_;
    std::vector<std::string> addresses_;
  DatabaseParameters db_params_;
  storehouse::StorageBackend *storage_;
  std::map<std::string, TableMetadata> table_metas_;
  proto::JobParameters job_params_;
  ProgressBar *bar_;

  i64 total_samples_used_;
  i64 total_samples_;

  std::mutex work_mutex_;
  i64 next_task_;
  i64 num_tasks_;
  std::unique_ptr<TaskSampler> task_sampler_;
  i64 samples_left_;
  Result task_result_;
};

proto::Master::Service *get_master_service(DatabaseParameters &param) {
  return new MasterImpl(param);
}
}
}
