#include <linux/module.h>
#include <linux/io.h>

static void __iomem *my_ipi_base;

extern void * nctuss_get_my_ipi_base(void);

static int __init led_init(void)
{
	my_ipi_base = nctuss_get_my_ipi_base();
	if(!my_ipi_base) {
		printk(KERN_ALERT "nctuss_emudisk: ioremap for my_ipi_base at 0x6A000000 failed\n");
		return -1;
	}


	writel(0x1, my_ipi_base + 8);
	writel(0x1, my_ipi_base + 12);

	return 0;
}

static void __exit led_exit(void)
{
	writel(0x0, my_ipi_base + 8);
	writel(0x0, my_ipi_base + 12);
}
module_init(led_init);
module_exit(led_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ming-Ju Wu");
MODULE_DESCRIPTION("NCTU LED");

