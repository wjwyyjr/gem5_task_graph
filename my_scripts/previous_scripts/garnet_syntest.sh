./build/X86/gem5.opt \
--debug-flags=GarnetSyntheticTraffic,Ruby,Cache,CachePort configs/example/garnet_synth_traffic.py \
--num-cpus=4 \
--num-dirs=4 \
--network=garnet2.0 \
--topology=Mesh_XY \
--mesh-rows=2  \
--injectionrate=0.01 \
--single-sender-id=0 \
--single-dest-id=1 \
--num-packets-max=5 
