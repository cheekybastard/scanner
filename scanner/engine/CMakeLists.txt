set(SOURCE_FILES
  master.cpp
  worker.cpp
  ingest.cpp
  load_worker.cpp
  evaluate_worker.cpp
  save_worker.cpp
  sampling.cpp
  sampler.cpp
  db.cpp
  kernel_registry.cpp
  op_registry.cpp)

add_library(engine OBJECT
  ${SOURCE_FILES})
