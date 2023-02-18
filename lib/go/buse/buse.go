// Copyright (C) 2021-2022 Vojtech Aschenbrenner <v@asch.cz>

package buse

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io/ioutil"
	"os"
	"runtime"
	"sync"
	"syscall"

	"golang.org/x/sys/unix"
)

const (
	// Character device for buse device %d and read queue %d.
	buseReadPathFmt = "/dev/buse%d-r%d"

	// Character device for buse device %d and write queue %d.
	buseWritePathFmt = "/dev/buse%d-w%d"

	// Path to the configfs directory.
	configFsPath = "/sys/kernel/config/buse"

	// Size of write request in write queue.
	writeRequestSize = 16

	// Size of read request in read queue.
	readRequestSize = 24
)

// Provides functions which are called by buse as a reaction to the received
// command.
type BuseReadWriter interface {
	// BuseRead should read the extent starting at the given sector with
	// the given length. The read data should be written to the provided
	// sice. The chunk is guaranteed to have sufficient capacity to hold
	// the data.
	//
	// This method is called by the BUSE library in response to a read
	// request received from the kernel driver.
	BuseRead(sector, length int64, chunk []byte) error

	// BuseWrite should handle all writes stored in the given chunk. The
	// first argument holds the number of writes in the chunk.
	//
	// This method is called by the BUSE library in response to a write
	// or flush request received from the kernel driver.
	BuseWrite(writes int64, chunk []byte) error

	// BusePreRun is called immediately before the device is started.
	BusePreRun()

	// BusePostRemove is called after the device is removed.
	BusePostRemove()
}

// Options for created buse device.
type Options struct {
	// Durable. If it is set to false then a weak flush mode is enabled.
	Durable bool

	// WriteChunkSize. Size (in bytes) of the chunk where write requests and
	// corresponding data are batched. Chunk is the smallest unit of data sent
	// to userspace (with the exception of flushes). Needs to be divisible by BlockSize.
	WriteChunkSize int64

	// BlockSize. The minimum block size (in bytes) the block device can handle.
	// Only 512 and 4096 are currently accepted (this is not a hard requirement
	// and may be changed in the future, the restriction was imposed to
	// minimize the testing surface).
	BlockSize int64

	// IOMin. Minimal IO.
	IOMin int64

	// IOOpt. Optimal IO.
	IOOpt int64

	// Threads. Number of threads (more precisely goroutines) to use. This
	// implies the number of hardware queues (hw_queues).
	Threads int

	// Major. The numeric suffix for the created block device. I.e. if Major = 7
	//then device /dev/buse7 is created with configfs directory in /sys/config/
	//kernel/buse/7.
	Major int64

	// WriteShmSize. Size (in bytes) of the shared memory between the kernel
	// and userspace of each write queue. Data of write chunks are stored in
	// the shared memory, hence size of the shared memory limits the number
	// of write requests pending in the userspace.
	WriteShmSize int64

	// ReadShmSize. Size (in bytes) of the shared memory between the kernel
	// and userspace of each read queue. For every read request, an appropriately-
	// sized memory slot is reserved in the shared memory of the read queue,
	// and its offset is sent to the userspace along with the read request.
	ReadShmSize int64

	// Size (in bytes) of the block device. Note that if the userspace driver
	// and file system support it, block devices can change its size while in use.
	Size int64

	// CollisionArea. Size (in bytes) of the area sharing the same write
	// sequential counter. This is mainly for performance tuning.
	CollisionArea int64

	// QueueDepth. Depth of each hardware queue specified as the number of
	// outstanding requests.
	QueueDepth int64

	// Scheduler. If set to false, the I/O request should go directly to hardware
	// dispatch queues, bypassing the staging area of software queues. This
	// means no merging, reordering etc. of the I/O requests will be done.
	Scheduler bool

	// CPUsPerNode is the number of virtual CPUs allowed to process single hw queue.
	CPUsPerNode int
}

// Buse is a library wrapping the low level interaction with buse kernel module
// and provides simple API to for creating a block device in user space.
type Buse struct {
	ReadWriter BuseReadWriter
	Options    Options
}

// Returns new instance of Buse configured with options o.
func New(rw BuseReadWriter, o Options) (Buse, error) {
	buse := Buse{
		ReadWriter: rw,
		Options:    o,
	}

	err := buse.checkOptions()
	if err != nil {
		return Buse{}, err
	}

	err = buse.configure()
	if err != nil {
		return Buse{}, err
	}

	return buse, nil
}

// Returns total memory presented to the system.
func totalMemory() (uint64, error) {
	sysInfo := &syscall.Sysinfo_t{}

	if err := syscall.Sysinfo(sysInfo); err != nil {
		return 0, err
	}

	// On 32-bit architectures the result is uint, hence we need to type it
	// to uint64 to conform with function signature.
	totalMemory := uint64(sysInfo.Totalram) * uint64(sysInfo.Unit)

	return totalMemory, nil
}

// Validates passed options.
func (b *Buse) checkOptions() error {
	o := &b.Options

	if o.Threads == 0 || o.Threads > runtime.NumCPU() {
		o.Threads = runtime.NumCPU()
	}

	if o.CPUsPerNode == 0 || o.CPUsPerNode > runtime.NumCPU() {
		o.CPUsPerNode = runtime.NumCPU()
	}

	if o.IOMin == 0 {
		o.IOMin = o.BlockSize
	}

	if o.IOOpt == 0 {
		o.IOOpt = o.BlockSize
	}

	totalMem, err := totalMemory()
	if err != nil {
		return errors.New("cannot read total amount of ram")
	}

	neededMemory := uint64(o.Threads) * uint64(o.WriteShmSize+o.ReadShmSize)
	if neededMemory > totalMem {
		return errors.New("not enough memory")
	}

	if o.WriteShmSize%o.WriteChunkSize != 0 {
		return errors.New("write buffer size has to be a multiple of chunk size")
	}

	if o.BlockSize != 512 && o.BlockSize != 4096 {
		return errors.New("block size has to 512 or 4096")
	}

	if o.IOMin < o.BlockSize || o.IOMin%2 != 0 {
		return errors.New("minimal IO has to be at least a block size and a power of 2")
	}

	if o.IOOpt < o.BlockSize || o.IOOpt%2 != 0 {
		return errors.New("optimal IO has to be at least a block size and a power of 2")
	}

	if o.QueueDepth < 10 {
		return errors.New("queue depth should be at least 10")
	}

	return nil
}

// Performs configuration of the block device which is just being created. It
// configures buse device via configs according to the options passed to the
// New() function. When configuration succeed the device is power on.
func (b *Buse) configure() error {
	var noScheduler int64
	if !b.Options.Scheduler {
		noScheduler = 1
	}

	configFsPath := fmt.Sprint(configFsPath, "/", b.Options.Major)
	if _, err := os.Stat(configFsPath); !os.IsNotExist(err) {
		return errors.New(fmt.Sprintf("Device buse%d already exists!", b.Options.Major))
	}

	if err := os.Mkdir(configFsPath, 0755); err != nil {
		return err
	}

	kernelParams := map[string]int64{
		"size":                b.Options.Size,
		"collision_area_size": int64(b.Options.CollisionArea),
		"read_shm_size":       int64(b.Options.ReadShmSize),
		"write_shm_size":      int64(b.Options.WriteShmSize),
		"write_chunk_size":    int64(b.Options.WriteChunkSize),
		"hw_queues":           int64(b.Options.Threads),
		"blocksize":           int64(b.Options.BlockSize),
		"io_min":              int64(b.Options.IOMin),
		"io_opt":              int64(b.Options.IOOpt),
		"queue_depth":         int64(b.Options.QueueDepth),
		"no_scheduler":        noScheduler,
	}

	for variable, value := range kernelParams {
		if err := b.setConfig(variable, value); err != nil {
			return err
		}
	}

	if err := b.setConfig("power", 1); err != nil {
		return err
	}

	return nil
}

// Opens control file and mmap it. Returns file and mmapped memory.
func openAndMmapControlFile(chardev string, shm_size int) (*os.File, []byte, error) {
	f, err := os.OpenFile(chardev, os.O_RDWR, 0644)
	if err != nil {
		return nil, nil, err
	}

	shmem, err := syscall.Mmap(int(f.Fd()), 0, shm_size,
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		f.Close()
		return nil, nil, err
	}

	return f, shmem, err
}

// Parses request reading from write queue character device.
func (b *Buse) parseWriteRequest(request []byte) ([]byte, uint64, uint64) {
	raw := make([]byte, 8)
	copy(raw, request[:8])
	offset := binary.LittleEndian.Uint64(raw)
	writesLen := binary.LittleEndian.Uint64(request[8:16])

	return raw, offset, writesLen
}

// Parses request reading from read queue character device.
func (b *Buse) parseReadRequest(request []byte) ([]byte, uint64, uint64, uint64) {
	raw := make([]byte, 8)
	copy(raw, request[16:24])
	offset := binary.LittleEndian.Uint64(raw)

	sector := binary.LittleEndian.Uint64(request[:8]) * 512 / uint64(b.Options.BlockSize)
	length := binary.LittleEndian.Uint64(request[8:16]) * 512 / uint64(b.Options.BlockSize)

	return raw, offset, sector, length
}

// True if the request means termination of the device.
func isTermination(offset uint64) bool {
	return offset == ^uint64(0)
}

// True if the request is flush.
func isFlush(offset uint64) bool {
	return offset > (1 << 32)
}

func (b *Buse) bindToLocalNumaNode(cpuId int) {
	localNode := cpuId / b.Options.CPUsPerNode
	firstCpu := localNode * b.Options.CPUsPerNode
	lastCpu := firstCpu + b.Options.CPUsPerNode - 1

	cpuSet := unix.CPUSet{}
	cpuSet.Zero()

	for c := firstCpu; c <= lastCpu; c++ {
		cpuSet.Set(c)
	}

	unix.SchedSetaffinity(0, &cpuSet)
}

// Infinite loop reading from write queue character device and calling
// BuseWrite() callback provided by calling application. When the BuseWrite()
// returns then the batched write is confirmed to the kernel leading to the
// recycling of the buffer in shared memory.
func (b *Buse) writer(chardev string, wgFunc *sync.WaitGroup, shm_size int) {
	defer wgFunc.Done()

	var major, cpuId int
	fmt.Sscanf(chardev, buseWritePathFmt, &major, &cpuId)
	b.bindToLocalNumaNode(cpuId)

	controlFile, shmem, err := openAndMmapControlFile(chardev, shm_size)
	if err != nil {
		panic(err)
	}
	defer controlFile.Close()
	defer syscall.Munmap(shmem)

	requestBuffer := make([]byte, writeRequestSize)
	wg := sync.WaitGroup{}
	for {
		_, err := controlFile.Read(requestBuffer)
		if err != nil {
			continue
		}

		offsetRaw, offset, writesLen := b.parseWriteRequest(requestBuffer)

		if isTermination(offset) {
			wg.Wait()
			return
		}

		if isFlush(offset) {
			if b.Options.Durable {
				wg.Wait()
			}
			controlFile.Write(offsetRaw)
			continue
		}

		dataRegion := shmem[offset : offset+uint64(b.Options.WriteChunkSize)]
		wg.Add(1)
		go func() {
			defer wg.Done()

			err := b.ReadWriter.BuseWrite(int64(writesLen), dataRegion)
			if err != nil {
				fmt.Fprintf(os.Stderr, "Chunk write (%d writes) failed!\n", writesLen)
				fmt.Fprint(os.Stderr, err)
			}

			n, err := controlFile.Write(offsetRaw)
			if err != nil {
				fmt.Fprint(os.Stderr, "Read ack error, n =", n, "err=", err.Error())
				fmt.Fprint(os.Stderr, err)
			}
		}()
	}
}

// Infinite loop reading from read queue character device and calling
// BuseRead() callback provided by calling application. When the BuseRead()
// returns then the read request is acknowledged to the kernel.
func (b *Buse) reader(chardev string, wgFunc *sync.WaitGroup, shm_size int) {
	defer wgFunc.Done()

	var major, cpuId int
	fmt.Sscanf(chardev, buseReadPathFmt, &major, &cpuId)
	b.bindToLocalNumaNode(cpuId)

	controlFile, shmem, err := openAndMmapControlFile(chardev, shm_size)
	if err != nil {
		panic(err)
	}
	defer controlFile.Close()
	defer syscall.Munmap(shmem)

	requestBuffer := make([]byte, readRequestSize)
	var wg sync.WaitGroup
	for {
		_, err := controlFile.Read(requestBuffer)
		if err != nil {
			continue
		}

		offsetRaw, offset, sector, length := b.parseReadRequest(requestBuffer)

		if isTermination(offset) {
			wg.Wait()
			return
		}

		size := int64(length) * b.Options.BlockSize
		dataRegion := shmem[int64(offset) : int64(offset)+size]

		wg.Add(1)
		go func() {
			defer wg.Done()

			err := b.ReadWriter.BuseRead(int64(sector), int64(length), dataRegion)
			if err != nil {
				fmt.Fprint(os.Stderr, err)
			}

			_, err = controlFile.Write(offsetRaw)
			if err != nil {
				fmt.Fprint(os.Stderr, err)
			}
		}()
	}
}

// Bind all the control queues and start processing read and write commands.
// This is done via multiple readers and writers. One worker per queue.
func (b *Buse) Run() {
	b.ReadWriter.BusePreRun()

	var wg sync.WaitGroup
	wg.Add(int(b.Options.Threads) * 2)
	for i := 0; i < int(b.Options.Threads); i++ {
		w := fmt.Sprintf(buseWritePathFmt, b.Options.Major, i)
		r := fmt.Sprintf(buseReadPathFmt, b.Options.Major, i)

		go b.writer(w, &wg, int(b.Options.WriteShmSize))
		go b.reader(r, &wg, int(b.Options.ReadShmSize))
	}
	wg.Wait()
}

// Write value to configfs variable.
func (b *Buse) setConfig(variable string, value int64) error {
	configFsPath := fmt.Sprint(configFsPath, "/", b.Options.Major, "/", variable)
	byteValue := []byte(fmt.Sprint(value))

	err := ioutil.WriteFile(configFsPath, byteValue, 0644)

	return err
}

// Stop buse device. All requests are refused but the device is still visible
// and can be started again.
func (b *Buse) StopDevice() error {
	err := b.setConfig("power", 0)
	return err
}

// Remove the device. The device is unregistered as block device.
func (b *Buse) RemoveDevice() error {
	err := syscall.Rmdir(fmt.Sprint(configFsPath, "/", b.Options.Major))
	b.ReadWriter.BusePostRemove()
	return err
}
