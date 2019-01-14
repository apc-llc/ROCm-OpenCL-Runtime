# ROCm OpenCL™ Compatible Runtime 

Developer preview Version 2 of the new 

* OpenCL 1.2 compatible language runtime
* OpenCL 2.0 compatible kernel language support with OpenCL 1.2 compatible runtime


## Build for debugging

```
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j12
```

## Build for release

```
mkdir build
cd build
cmake ..
make -j12
```

## Testing

```
./bin/icd_loader_test
./bin/clinfo
```

OpenCL™ is registered Trademark of Apple

