#include "DiskPlotPhase1.h"
#include "pos/chacha8.h"
#include "Util.h"
#include "util/Log.h"

// Test
#include "io/FileStream.h"
#include "SysHost.h"

struct WriteFileJob
{
    const char* filePath;

    size_t      size  ;
    size_t      offset;
    byte*       buffer;
    bool        success;

    static void Run( WriteFileJob* job );
};


//-----------------------------------------------------------
DiskPlotPhase1::DiskPlotPhase1( DiskPlotContext& cx )
    : _cx( cx )
{}

//-----------------------------------------------------------
void DiskPlotPhase1::Run()
{
    GenF1();
}

//-----------------------------------------------------------
void DiskPlotPhase1::GenF1()
{
    DiskPlotContext& cx   = _cx;
    ThreadPool&      pool = *cx.threadPool;

    // Prepare ChaCha key
    byte key[32] = { 1 };
    memcpy( key + 1, cx.plotId, 31 );

    // Prepare jobs
    const uint threadCount = pool.ThreadCount();

    const uint64 entryCount      = 1ull << _K;
    const size_t entryTotalSize  = entryCount * sizeof( uint32 );
    const size_t chaChaBlockSize = kF1BlockSizeBits / 8u;
    const uint32 entriesPerBlock = chaChaBlockSize / sizeof( uint32 );

    const uint   chunkCount      = (uint)(entryTotalSize / cx.diskFlushSize);
    const uint32 blockCount      = (uint32)(entryCount / chaChaBlockSize);
    const uint32 blocksPerChunk  = (uint32)(cx.diskFlushSize / chaChaBlockSize);
    const uint32 blocksPerThread = blocksPerChunk / threadCount;
    
    uint32 trailingBlocks = blocksPerChunk - ( blocksPerThread * threadCount );

    // #TODO: Ensure each thread has at least one block.
    ASSERT( blocksPerThread > 0 );

    uint  x      = 0;
    byte* buffer = _cx.workBuffer;

    MTJobRunner<GenF1Job> f1Job( pool );

    for( uint i = 0; i < threadCount; i++ )
    {
        GenF1Job& job = f1Job[i];
        job.key          = key;
        job.buffer       = buffer;
        job.blockCount   = blocksPerThread;
        job.chunkCount   = chunkCount;
        job.x            = x;

        if( trailingBlocks > 0 )
        {
            job.blockCount++;
            trailingBlocks--;
        }

        x      += job.blockCount * entriesPerBlock;
        buffer += job.blockCount * chaChaBlockSize;
    }

    Log::Line( "Generating f1..." );
    const double elapsed = f1Job.Run();
    Log::Line( "Finished f1 generation in %.2lf seconds. ", elapsed );
}


//-----------------------------------------------------------
void GenF1Job::Run()
{
    const uint32 blockCount   = this->blockCount;
    const uint32 chunkCount   = this->chunkCount;
    
    uint32 x = this->x;

    chacha8_ctx chacha;
    chacha8_keysetup( &chacha, key, 256, NULL );

    for( uint i = 0; i < chunkCount; i++ )
    {
        chacha8_get_keystream( &chacha, x, blockCount, (byte*)buffer );
        SyncThreads();

        // #TODO:
        // Count each bucket's entries
        // Sort in the buffer itself, per bucket
        // Dispatch sorted buffer to the wite queue
        // Take new buffer from read queue (may suspend the thread
        // if there's no more buffers)
    }
}

//-----------------------------------------------------------
void WriteFileJob::Run( WriteFileJob* job )
{
    job->success = false;

    FileStream file;
    if( !file.Open( job->filePath, FileMode::Open, FileAccess::Write, FileFlags::NoBuffering | FileFlags::LargeFile ) )
        return;

    ASSERT( job->offset == ( job->offset / file.BlockSize() ) * file.BlockSize() );

    if( !file.Seek( (int64)job->offset, SeekOrigin::Begin ) )
        return;

    // Begin writing at offset
    size_t sizeToWrite = job->size;
    byte*  buffer      = job->buffer;

    while( sizeToWrite )
    {
        const ssize_t sizeWritten = file.Write( buffer, sizeToWrite );
        if( sizeWritten < 1 )
            return;

        ASSERT( (size_t)sizeWritten >= sizeToWrite );
        sizeToWrite -= (size_t)sizeWritten;
    }

    // OK
    job->success = true;
}


//-----------------------------------------------------------
void TestWrites()
{
    // Test file
    const char* filePath = "E:/bbtest.data";

    FileStream file;
    if( !file.Open( filePath, FileMode::Create, FileAccess::Write, FileFlags::LargeFile | FileFlags::NoBuffering ) )
        Fatal( "Failed to open file." );

    const size_t blockSize = file.BlockSize();

    byte key[32] = {
        22, 24, 11, 3, 1, 15, 11, 6, 23, 22,
        22, 24, 11, 3, 1, 15, 11, 6, 23, 22,
        22, 24, 11, 3, 1, 15, 11, 6, 23, 22, 5, 28
    };

    const uint   chachaBlockSize = 64;

    const uint   k          = 30;
    const uint64 entryCount = 1ull << k;
    const uint   blockCount = (uint)( entryCount / chachaBlockSize );
    
    SysHost::SetCurrentThreadAffinityCpuId( 0 );

    uint32* buffer;

    {
        Log::Line( "Allocating %.2lf MB buffer...", (double)( entryCount * sizeof( uint32 ) ) BtoMB );
        auto timer = TimerBegin();

        buffer = (uint32*)SysHost::VirtualAlloc( entryCount * sizeof( uint32 ), true );
        FatalIf( !buffer, "Failed to allocate buffer." );

        double elapsed = TimerEnd( timer );
        Log::Line( "Finished in %.2lf seconds.", elapsed );
    }

    {
        chacha8_ctx chacha;
        ZeroMem( &chacha );

        Log::Line( "Generating ChaCha..." );
        auto timer = TimerBegin();

        chacha8_keysetup( &chacha, key, 256, NULL );
        chacha8_get_keystream( &chacha, 0, blockCount, (byte*)buffer );

        double elapsed = TimerEnd( timer );
        Log::Line( "Finished in %.2lf seconds.", elapsed );
    }

    bool singleThreaded = false;
    
    if( singleThreaded )
    {
        Log::Line( "Started writing to file..." );

        const size_t sizeWrite = entryCount * sizeof( uint );
        const size_t blockSize = file.BlockSize();
        
        size_t blocksToWrite = sizeWrite / blockSize;

        auto timer = TimerBegin();

        do
        {
            ssize_t written = file.Write( buffer, blocksToWrite * blockSize );
            FatalIf( written <= 0, "Failed to write to file." );

            size_t blocksWritten = (size_t)written / blockSize;
            ASSERT( blocksWritten <= blocksToWrite );

            blocksToWrite -= blocksWritten;
        } while( blocksToWrite > 0 );
        
        double elapsed = TimerEnd( timer );
        Log::Line( "Finished in %.2lf seconds.", elapsed );
    }
    else
    {
        const uint threadCount = 1;

        WriteFileJob jobs[threadCount];

        const size_t blockSize       = file.BlockSize();
        const size_t sizeWrite       = entryCount * sizeof( uint );
        const size_t totalBlocks     = sizeWrite / blockSize;
        const size_t blocksPerThread = totalBlocks / threadCount;
        const size_t sizePerThread   = blocksPerThread * blockSize;

        const size_t trailingSize    = sizeWrite - (sizePerThread * threadCount);

        byte* buf = (byte*)buffer;

        for( uint i = 0; i < threadCount; i++ )
        {
            WriteFileJob& job = jobs[i];

            job.filePath = filePath;
            job.success  = false;
            job.size     = sizePerThread;
            job.offset   = sizePerThread * i;
            job.buffer   = buf + job.offset;
        }

        jobs[threadCount-1].size += trailingSize;
        
        ThreadPool pool( threadCount );

        Log::Line( "Writing to file with %u threads.", threadCount );
        
        auto timer = TimerBegin();
        pool.RunJob( WriteFileJob::Run, jobs, threadCount );
        double elapsed = TimerEnd( timer );

        const double bytesPerSecond = sizeWrite / elapsed;
        Log::Line( "Finished writing to file in %.2lf seconds @ %.2lf MB/s.", elapsed, ((double)bytesPerSecond) BtoMB );
    }
}
