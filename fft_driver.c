#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/platform_device.h>
#include <linux/dmaengine.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timekeeping.h>

#define FFT_BLOCK_MAJOR   69
#define FFT_BLKDEV_NAME   "fft_blkdev"
#define FFT_LEN           1024
#define BUFFER_SIZE       (sizeof(u32) * (FFT_LEN))
#define ITERS             100000

static dma_addr_t dma_src, dma_dst;
static struct dma_chan * tx_chan, * rx_chan;
static struct dma_slave_config fft_tx_dma_config;
static struct dma_slave_config fft_rx_dma_config;
volatile static u32 * tx_data, * rx_data;

struct complex
{
  short real;
  short imag;
};

static void fft_tx_callback(void * completion)
{
  //printk( KERN_INFO "tx complete\n");
  complete(completion);
}

static void fft_rx_callback(void * completion)
{
  //printk( KERN_INFO "rx complete\n");
  complete(completion);
}

// static int fft_blkdev_init(void)
// {
//   int status;
//   status = register_blkdev(FFT_BLOCK_MAJOR, FFT_BLKDEV_NAME);
//   if(status < 0)
//   {
//     printk( KERN_ERR "failed to register fft_blkdev\n");
//     return -EBUSY;
//   }
// }
//
// static void fft_blkdev_exit(void)
// {
//   unregister_blkdev(FFT_BLOCK_MAJOR, FFT_BLKDEV_NAME);
// }

static int fft_probe(struct platform_device *pdev)
{
  int status;
  int err = 0, i;
  dma_addr_t tx_addr, rx_addr;
  struct dma_async_tx_descriptor * txd = NULL;
  struct dma_async_tx_descriptor * rxd = NULL;
  struct scatterlist tx_sg[1];
  struct scatterlist rx_sg[1];
  struct completion tx_cmp;
  struct completion rx_cmp;
  dma_cookie_t tx_cookie;
  dma_cookie_t rx_cookie;
  unsigned long tx_tmo = msecs_to_jiffies(30000);
  unsigned long rx_tmo = msecs_to_jiffies(300000);

  /* Profile FFT operation speed */
  struct timespec start;
  struct timespec end;

  pr_info("fft_probe\n");

  /*
    https://forums.xilinx.com/t5/Embedded-Linux/AXI-DMA-request-channel-problem/td-p/678801
  */
  pr_info("dma_request_slave_channel() [tx]\n");
  tx_chan = dma_request_slave_channel(&pdev->dev, "axidma0");
  if(tx_chan)
  {
    printk( KERN_INFO "found tx device\n");
  }
  else
  {
    printk( KERN_INFO "did not find tx device\n");
    err = -1;
    goto exit_fft_probe;
  }

  pr_info("dma_request_slave_channel() [rx]\n");
  rx_chan = dma_request_slave_channel(&pdev->dev, "axidma1");
  if(rx_chan)
  {
    printk( KERN_INFO "found rx device\n");
  }
  else
  {
    printk( KERN_INFO "did not find rx device\n");
    err = -1;
    goto release_tx_chan;
  }

  // err = dmaengine_slave_config(tx_chan, &fft_tx_dma_config);
  // if(err)
  // {
  //   printk( KERN_INFO "failed to configure tx channel, err = %d\n", err);
  //   goto release_rx_chan;
  // }
  //
  // err = dmaengine_slave_config(rx_chan, &fft_rx_dma_config);
  // if(err)
  // {
  //   printk( KERN_INFO "failed to configure rx channel, err = %d\n", err);
  //   goto release_rx_chan;
  // }

  tx_data = (u32 *)kcalloc(FFT_LEN, sizeof(u32), GFP_DMA);
  if(NULL == tx_data)
  {
    err = -1;
    goto release_rx_chan;
  }

  rx_data = (u32 *)kcalloc(FFT_LEN, sizeof(u32), GFP_DMA);
  if(NULL == rx_data)
  {
    err = -1;
    goto free_tx_buffer;
  }

  for(i=0; i < FFT_LEN; ++i)
  {
    if(i%2==0)
      tx_data[i] = 0x00000001;
    else
      tx_data[i] = 0x00010000;
  }

  getnstimeofday(&start);

  for(i=0; i < ITERS; ++i)
  {
    tx_addr = dma_map_single(tx_chan->device->dev, tx_data, FFT_LEN * sizeof(u32), DMA_MEM_TO_DEV);
    //printk( KERN_INFO "tx_addr = %x\n", tx_addr);

    rx_addr = dma_map_single(rx_chan->device->dev, rx_data, FFT_LEN * sizeof(u32), DMA_DEV_TO_MEM);
    //printk( KERN_INFO "rx_addr = %x\n", rx_addr);

    sg_init_table(tx_sg, 1);
    sg_init_table(rx_sg, 1);

    sg_dma_address(&tx_sg[0]) = tx_addr;
    sg_dma_address(&rx_sg[0]) = rx_addr;

    sg_dma_len(&tx_sg[0]) = FFT_LEN * sizeof(u32);
    sg_dma_len(&rx_sg[0]) = FFT_LEN * sizeof(u32);

    txd = tx_chan->device->device_prep_slave_sg(tx_chan, tx_sg, 1, DMA_MEM_TO_DEV, 0, NULL);
    rxd = rx_chan->device->device_prep_slave_sg(rx_chan, rx_sg, 1, DMA_DEV_TO_MEM, 0, NULL);

    if( !txd || !rxd )
    {
      printk( KERN_INFO "device_prep_slave_sg() failed\n");
      err = -1;
      goto unmap_dma_buffers;
    }

    init_completion(&rx_cmp);
    rxd->callback = fft_rx_callback;
    rxd->callback_param = &rx_cmp;
    rx_cookie = rxd->tx_submit(rxd);

    init_completion(&tx_cmp);
    txd->callback = fft_tx_callback;
    txd->callback_param = &tx_cmp;
    tx_cookie = txd->tx_submit(txd);

    if (dma_submit_error(rx_cookie) || dma_submit_error(tx_cookie))
    {
      printk( KERN_INFO "submit error\n");
      goto unmap_dma_buffers;
    }


    dma_async_issue_pending(rx_chan);
    dma_async_issue_pending(tx_chan);

    tx_tmo = wait_for_completion_timeout(&tx_cmp, tx_tmo);
    rx_tmo = wait_for_completion_timeout(&rx_cmp, rx_tmo);

    if(tx_tmo == 0 || rx_tmo == 0)
      goto timeout;

    dma_unmap_single(tx_chan->device->dev, tx_addr, FFT_LEN * sizeof(u32), DMA_MEM_TO_DEV);
    dma_unmap_single(rx_chan->device->dev, rx_addr, FFT_LEN * sizeof(u32), DMA_DEV_TO_MEM);

  }

  getnstimeofday(&end);
  printk( KERN_INFO "execution time : %ld:%ld\n", (end.tv_sec - start.tv_sec)/60, (end.tv_sec - start.tv_sec)%60);

  // for(i=0; i < FFT_LEN; ++i)
  // {
  //   short real, imag;
  //   real = (short)(rx_data[i] & 0xFFFF);
  //   imag = (short)((rx_data[i] >> 16)&0xFFFF);
  //   printk( KERN_INFO "rx[%d] = %d + %d j\n", i, real, imag);
  // }

  return 0;

  timeout:
    printk( KERN_INFO "timed out\n");
  unmap_dma_buffers:
    dma_unmap_single(tx_chan->device->dev, tx_addr, FFT_LEN * sizeof(u32), DMA_MEM_TO_DEV);
    dma_unmap_single(rx_chan->device->dev, rx_addr, FFT_LEN * sizeof(u32), DMA_DEV_TO_MEM);
  free_rx_buffer:
    kfree(rx_data);
    rx_data = NULL;
  free_tx_buffer:
    kfree(tx_data);
    tx_data = NULL;
  release_rx_chan:
    dma_release_channel(rx_chan);
  release_tx_chan:
    dma_release_channel(tx_chan);
  exit_fft_probe:
  return err;
}

static int fft_remove(struct platform_device *pdev)
{
  pr_info("fft_remove\n");

  if(rx_data)
  {
    kfree(rx_data);
    rx_data = NULL;
  }

  if(tx_data)
  {
    kfree(tx_data);
    tx_data = NULL;
  }

  dma_release_channel(rx_chan);
  dma_release_channel(tx_chan);

  return 0;
}

static const struct of_device_id fft_of_ids[] =
{
  { .compatible = "xlnx,axi-dma-test-1.00.a",},
  {}
};

static struct platform_driver fft_driver =
{
  .driver =
  {
    .name = "fft_driver",
    .of_match_table = fft_of_ids,
  },
  .probe = fft_probe,
  .remove = fft_remove,
};

static int __init fft_init(void)
{
  return platform_driver_register(&fft_driver);
}

static void __exit fft_exit(void)
{
  platform_driver_unregister(&fft_driver);
}

module_init(fft_init);
module_exit(fft_exit);

MODULE_AUTHOR("Alex Hansen");
MODULE_DESCRIPTION("Xilinx AXI DMA driven FFT processor");
MODULE_LICENSE("GPL v2");
