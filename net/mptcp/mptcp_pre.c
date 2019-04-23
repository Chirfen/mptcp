/* MPTCP pre information.  */

#include <linux/module.h>
#include <net/mptcp.h>

#define ADDR_CHAR_SIZE 20
#define ROOT_CHAR_SIZE 20
#define FILENAME_CHAR_SIZE 60

void timer_function_update(unsigned long addr_to_sk)
{
    
    //根据pathindedx获得struct sock * sk
    //判断当前是否down
        //当前down，是否会再次up
            //还会up，当前时间是否超过up时间
                //
            
}

int mptcp_pre_from_file_init(struct sock * sk){
    struct mptcp_pre * pre = ( struct mptcp_pre *)vmalloc(sizeof(struct mptcp_pre));
    struct tcp_sock * tp =  tcp_sk(sk);
    tp->mptcp->pre = pre;

    //获取sock的源和目的IP地址
    char * daddr = (char *)vmalloc( ADDR_CHAR_SIZE * sizeof(char));
    char * saddr = (char *)vmalloc( ADDR_CHAR_SIZE * sizeof(char));
    

    inet_ntop(AF_INET,sk->sk_daddr,daddr,sizeof(daddr));
    inet_ntop(AF_INET,sk->sk_rcv_saddr,saddr,sizeof(saddr));

    #ifdef CONFIG_MPTCP_DEBUG
    printk(KERN_DEBUG "pre daddr:%s",daddr);
    printk(KERN_DEBUG "pre saddr:%s",saddr);
    #endif

    //打开目录中的文件(源IP,目的IP.pre)
    char * filename = (char *)vmalloc( FILENAME_CHAR_SIZE * sizeof(char));
    char * root = (char *)vmalloc( ROOT_CHAR_SIZE * sizeof(char));
    strcpy(root,"/etc/mptcp");
    sprintf(filename,"%s/%s,%s.pre",root,saddr,daddr); //生成完整的文件名

    #ifdef CONFIG_MPTCP_DEBUG
    printk(KERN_DEBUG "pre filename:%s",filename);
    #endif

    struct file * fp;
    fp = filp_open(filename,O_RDWR | O_CREAT,0644);
    if(IS_ERR(fp)){
        printk("open or create file error/n");
        return -1;
    }

    //设置相关参数，并设置定时器
    
    //(定时器处理函数的参数可以考虑将 sk 指针 强制转换为unsigned long)

    //析构
    vfree(daddr);
    vfree(saddr);
    vfree(filename);
    
}
int mptcp_pre_from_file_exit(struct sock*sk){
    //删除timer
    //删除vmalloc的空间--vfree(pre);
}

int mptcp_pre_from_option_init(struct sock*sk);
int mptcp_pre_from_option_exit(struct sock*sk);


/*
static struct mptcp_sched_ops mptcp_sched_rr = {
	.get_subflow = rr_get_available_subflow,
	.next_segment = mptcp_rr_next_segment,
	.name = "roundrobin",
	.owner = THIS_MODULE,
};

static int __init pre_register(void)
{
	BUILD_BUG_ON(sizeof(struct rrsched_priv) > MPTCP_SCHED_SIZE);

	if (mptcp_register_scheduler(&mptcp_sched_rr))
		return -1;

	return 0;
}

static void __exit rr_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_sched_rr);
}

module_init(rr_register);
module_exit(rr_unregister);


*/

MODULE_AUTHOR("Chirfen Zhang");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP PRE INFO");
MODULE_VERSION("0.1");
