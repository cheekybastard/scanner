#include "scanner/api/database.h"
#include "scanner/engine/op_info.h"
#include "scanner/engine/op_registry.h"
#include "scanner/util/common.h"

#include <boost/python.hpp>
#include <boost/python/stl_iterator.hpp>

namespace scanner {

namespace py = boost::python;

std::string get_include() {
  // This variable is filled in at compile time by CMake.
  return "@dirs@";
}

std::string other_flags() {
#ifdef HAVE_CUDA
  return "-DHAVE_CUDA";
#else
  return "";
#endif
}

template <typename T>
inline std::vector<T> to_std_vector(const py::object &iterable) {
  return std::vector<T>(py::stl_input_iterator<T>(iterable),
                        py::stl_input_iterator<T>());
}

template <class T> py::list to_py_list(std::vector<T> vector) {
  typename std::vector<T>::iterator iter;
  py::list list;
  for (iter = vector.begin(); iter != vector.end(); ++iter) {
    list.append(*iter);
  }
  return list;
}

py::list get_output_columns(const std::string &op_name) {
  internal::OpRegistry *registry = internal::get_op_registry();
  LOG_IF(FATAL, !registry->has_op(op_name))
      << "Op " << op_name << " does not exist.";
  internal::OpInfo *info = registry->get_op_info(op_name);
  return to_py_list(info->output_columns());
}

bool has_op(const std::string &name) {
  internal::OpRegistry *registry = internal::get_op_registry();
  return registry->has_op(name);
}

std::string default_machine_params_wrapper() {
  MachineParameters params = default_machine_params();
  proto::MachineParameters params_proto;
  params_proto.set_num_cpus(params.num_cpus);
  params_proto.set_num_load_workers(params.num_load_workers);
  params_proto.set_num_save_workers(params.num_save_workers);
  for (auto gpu_id : params.gpu_ids) {
    params_proto.add_gpu_ids(gpu_id);
  }

  std::string output;
  bool success = params_proto.SerializeToString(&output);
  LOG_IF(FATAL, !success) << "Failed to serialize machine params";
  return output;
}

void start_master_wrapper(Database& db) {
  db.start_master(default_machine_params());
}

void start_worker_wrapper(Database& db, const std::string& params_s) {
  proto::MachineParameters params_proto;
  params_proto.ParseFromString(params_s);
  MachineParameters params;
  params.num_cpus = params_proto.num_cpus();
  params.num_load_workers = params_proto.num_load_workers();
  params.num_save_workers = params_proto.num_save_workers();
  for (auto gpu_id : params_proto.gpu_ids()) {
    params.gpu_ids.push_back(gpu_id);
  }

  db.start_worker(params);
}

py::list ingest_videos_wrapper(
  Database& db,
  const py::list table_names,
  const py::list paths) {
  std::vector<FailedVideo> failed_videos;
  db.ingest_videos(
    to_std_vector<std::string>(table_names),
    to_std_vector<std::string>(paths),
    failed_videos);
  return to_py_list<FailedVideo>(failed_videos);
}


BOOST_PYTHON_MODULE(scanner_bindings) {
  using namespace py;
  class_<Database, boost::noncopyable>(
    "Database", init<storehouse::StorageConfig*, const std::string&, const std::string&>())
    .def("ingest_videos", &Database::ingest_videos);
  class_<FailedVideo>("FailedVideo", no_init)
    .def_readonly("path", &FailedVideo::path)
    .def_readonly("message", &FailedVideo::message);
  def("start_master", start_master_wrapper);
  def("start_worker", start_worker_wrapper);
  def("ingest_videos", ingest_videos_wrapper);
  def("get_include", get_include);
  def("other_flags", other_flags);
  def("get_output_columns", get_output_columns);
  def("has_op", has_op);
  def("default_machine_params", default_machine_params_wrapper);
}
}
