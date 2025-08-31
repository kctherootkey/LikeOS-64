// LikeOS-64 - USB Mass Storage evolving implementation
#include "../../include/kernel/usb_msd.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/xhci_trb.h"
#include "../../include/kernel/block.h"
// ...existing code...
static block_device_t g_msd_block_dev; // single device instance
static void msd_issue_inquiry(xhci_controller_t* ctrl, usb_device_t* dev);
static void msd_issue_read_capacity(xhci_controller_t* ctrl, usb_device_t* dev);
static int msd_issue_read10(xhci_controller_t* ctrl, usb_device_t* dev, unsigned long lba, unsigned blocks);
static void msd_issue_test_unit_ready(xhci_controller_t* ctrl, usb_device_t* dev);
static void msd_issue_request_sense(xhci_controller_t* ctrl, usb_device_t* dev);
static void msd_recover_reset(xhci_controller_t* ctrl, usb_device_t* dev);
static void msd_recover_clear_stalls(xhci_controller_t* ctrl, usb_device_t* dev);
static void msd_fail_operation(xhci_controller_t* ctrl, const char* msg);
static void msd_progress(xhci_controller_t* ctrl); // forward declaration for cooperative polling

static usb_device_t* find_msd(xhci_controller_t* ctrl){
    for(int s=1; s<64; ++s){
        usb_device_t* dev = (usb_device_t*)ctrl->slot_device_map[s];
        if(!dev) continue;
        if(dev->configured && dev->class_code == USB_CLASS_MASS_STORAGE && dev->bulk_in_ep && dev->bulk_out_ep)
            return dev;
    }
    return 0;
}

static int bot_send_cbw(xhci_controller_t* ctrl, usb_device_t* dev, usb_msd_cbw_t* cbw){
    if(!dev->endpoints_configured){
        xhci_configure_mass_storage_endpoints(ctrl, dev);
        if(!dev->endpoints_configured) return -1;
    }
    // Allocate persistent buffer for CBW (31 bytes) align to 64 for simplicity
    if(!ctrl->msd_cbw_buf) ctrl->msd_cbw_buf = kcalloc(1, 64);
    if(!ctrl->msd_cbw_buf) return -1;
    // Extra guard: ensure structure size fits allocation and pointer sane
    if(sizeof(usb_msd_cbw_t) > 64){ kprintf("MSD: CBW size unexpected %u\n", (unsigned)sizeof(usb_msd_cbw_t)); return -1; }
    // Copy CBW
    for(unsigned i=0;i<sizeof(usb_msd_cbw_t);++i) ((uint8_t*)ctrl->msd_cbw_buf)[i] = ((uint8_t*)cbw)[i];
    // Enqueue OUT transfer (only CBW now; defer DATA/CSW until CBW completion event)
    if(xhci_enqueue_bulk_out(ctrl, dev, ctrl->msd_cbw_buf, sizeof(usb_msd_cbw_t)) != ST_OK) return -1;
    unsigned idx = (ctrl->bulk_out_enqueue - 1) % 16; // last enqueued index
    ctrl->msd_cbw_trb = &((xhci_trb_t*)ctrl->bulk_out_ring)[idx];
    ctrl->msd_cbw_phys = (unsigned long)MmGetPhysicalAddress((uint64_t)ctrl->msd_cbw_trb);
    // Reset stage TRB pointers for new operation
    ctrl->msd_data_trb = 0; ctrl->msd_csw_trb = 0; ctrl->msd_data_phys=0; ctrl->msd_csw_phys=0;
    ctrl->msd_state = 1; // CBW out in flight
    ctrl->msd_pending_data_buf = 0; ctrl->msd_pending_data_len = 0; ctrl->msd_need_csw = 0;
    return 0;
}

static int bot_read_csw(xhci_controller_t* ctrl, usb_device_t* dev, usb_msd_csw_t* csw){
    (void)dev; (void)csw; (void)ctrl; // placeholder
    // TODO: enqueue IN transfer TRBs on bulk IN ring
    return 0;
}

static unsigned next_tag(xhci_controller_t* ctrl){
    if(ctrl->msd_tag_counter==0 || ctrl->msd_tag_counter==0xFFFFFFFFu) ctrl->msd_tag_counter=1; else ctrl->msd_tag_counter++;
    return ctrl->msd_tag_counter;
}

static void msd_issue_inquiry(xhci_controller_t* ctrl, usb_device_t* dev){
    if(ctrl->msd_op) return;
    usb_msd_cbw_t cbw; for(int i=0;i<16;i++) cbw.cb[i]=0;
    unsigned tag = next_tag(ctrl); ctrl->msd_expected_tag = tag;
    cbw.signature=0x43425355; cbw.tag=tag; cbw.data_len=36; cbw.flags=0x80; cbw.lun=0; cbw.cb_len=6; cbw.cb[0]=SCSI_INQUIRY; cbw.cb[4]=36;
    if(!ctrl->msd_data_buf) ctrl->msd_data_buf = kcalloc(1,512);
    if(!ctrl->msd_csw_buf) ctrl->msd_csw_buf = kcalloc(1,64);
    if(bot_send_cbw(ctrl, dev, &cbw)==0){
        if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 36)==ST_OK){
            unsigned didx=(ctrl->bulk_in_enqueue-1)%16; ctrl->msd_data_trb=&((xhci_trb_t*)ctrl->bulk_in_ring)[didx]; ctrl->msd_data_phys=(unsigned long)MmGetPhysicalAddress((uint64_t)ctrl->msd_data_trb);
            ctrl->msd_expected_data_len=36; ctrl->msd_state=2; ctrl->msd_op=1; ctrl->msd_need_csw=1; // defer CSW until data complete
        }
    }
}

static void msd_issue_test_unit_ready(xhci_controller_t* ctrl, usb_device_t* dev){
    if(ctrl->msd_op) return;
    usb_msd_cbw_t cbw; for(int i=0;i<16;i++) cbw.cb[i]=0;
    unsigned tag = next_tag(ctrl); ctrl->msd_expected_tag = tag;
    cbw.signature=0x43425355; cbw.tag=tag; cbw.data_len=0; cbw.flags=0x80; cbw.lun=0; cbw.cb_len=6; cbw.cb[0]=0x00; // TEST UNIT READY
    if(bot_send_cbw(ctrl, dev, &cbw)==0){
        ctrl->msd_need_csw=1; ctrl->msd_op=4; // no data stage; CSW will be queued on CBW completion
    }
}

static void msd_issue_request_sense(xhci_controller_t* ctrl, usb_device_t* dev){
    if(ctrl->msd_op) return;
    usb_msd_cbw_t cbw; for(int i=0;i<16;i++) cbw.cb[i]=0;
    unsigned tag = next_tag(ctrl); ctrl->msd_expected_tag = tag;
    cbw.signature=0x43425355; cbw.tag=tag; cbw.data_len=18; cbw.flags=0x80; cbw.lun=0; cbw.cb_len=6; cbw.cb[0]=0x03; cbw.cb[4]=18; // REQUEST SENSE
    if(!ctrl->msd_data_buf) ctrl->msd_data_buf = kcalloc(1,512);
    if(bot_send_cbw(ctrl, dev, &cbw)==0){
        if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 18)==ST_OK){
            unsigned didx=(ctrl->bulk_in_enqueue-1)%16; ctrl->msd_data_trb=&((xhci_trb_t*)ctrl->bulk_in_ring)[didx]; ctrl->msd_data_phys=(unsigned long)MmGetPhysicalAddress((uint64_t)ctrl->msd_data_trb);
            ctrl->msd_expected_data_len=18; ctrl->msd_state=2; ctrl->msd_op=5; ctrl->msd_need_csw=1; // defer CSW
        }
    }
}

static void msd_fail_operation(xhci_controller_t* ctrl, const char* msg){
    kprintf("MSD: FAIL %s\n", msg);
    ctrl->msd_state=0; ctrl->msd_op=0;
}

static void msd_recover_clear_stalls(xhci_controller_t* ctrl, usb_device_t* dev){
    // CLEAR_FEATURE(ENDPOINT_HALT) for bulk IN and OUT endpoints
    if(!dev) return;
    uint8_t eps[2]; int epcount=0;
    if(dev->bulk_in_ep) eps[epcount++]=dev->bulk_in_ep;
    if(dev->bulk_out_ep) eps[epcount++]=dev->bulk_out_ep;
    for(int i=0;i<epcount;i++){
        uint8_t epaddr = eps[i];
        // Build setup packet (Host to Device, Standard, Endpoint)
        uint8_t bmRequestType = 0x02; // 0b00000010
        uint8_t bRequest = 1; // CLEAR_FEATURE
        uint16_t wValue = 0; // ENDPOINT_HALT
        uint16_t wIndex = epaddr;
        uint16_t wLength = 0;
        uint64_t setup_pkt = (uint64_t)bmRequestType | ((uint64_t)bRequest<<8) | ((uint64_t)wValue<<16) | ((uint64_t)wIndex<<32) | ((uint64_t)wLength<<48);
        if(ctrl->ep0_ring){
            xhci_trb_t* trb = (xhci_trb_t*)ctrl->ep0_ring;
            trb[0].param_lo = (uint32_t)(setup_pkt & 0xFFFFFFFFu);
            trb[0].param_hi = (uint32_t)(setup_pkt >> 32);
            trb[0].status = 0;
            trb[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_SETUP_STAGE) | XHCI_TRB_CYCLE;
            trb[1].param_lo = 0; trb[1].param_hi = 0; trb[1].status = 0; trb[1].control = 0; // no data
            trb[2].param_lo = 0; trb[2].param_hi = 0; trb[2].status = 0; trb[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_CYCLE;
            volatile uint32_t* db_slot = (volatile uint32_t*)(ctrl->doorbell_array + 1*4);
            *db_slot = 0;
            kprintf("MSD: CLEAR_FEATURE(HALT) ep=0x%02x issued\n", epaddr);
        }
    }
}

static void msd_recover_reset(xhci_controller_t* ctrl, usb_device_t* dev){
    if(!dev) return;
    // Class-specific Mass Storage Reset: bmRequestType=Class,Interface,HostToDevice (0x21) bRequest=0xFF wValue=0 wIndex=interface wLength=0
    uint8_t bmRequestType = 0x21;
    uint8_t bRequest = 0xFF;
    uint16_t wValue = 0;
    uint16_t wIndex = 0; // assume interface 0
    uint16_t wLength = 0;
    uint64_t setup_pkt = (uint64_t)bmRequestType | ((uint64_t)bRequest<<8) | ((uint64_t)wValue<<16) | ((uint64_t)wIndex<<32) | ((uint64_t)wLength<<48);
    if(ctrl->ep0_ring){
        xhci_trb_t* trb = (xhci_trb_t*)ctrl->ep0_ring;
        trb[0].param_lo = (uint32_t)(setup_pkt & 0xFFFFFFFFu);
        trb[0].param_hi = (uint32_t)(setup_pkt >> 32);
        trb[0].status = 0;
        trb[0].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_SETUP_STAGE) | XHCI_TRB_CYCLE;
        trb[1].param_lo = 0; trb[1].param_hi = 0; trb[1].status = 0; trb[1].control = 0;
        trb[2].param_lo = 0; trb[2].param_hi = 0; trb[2].status = 0; trb[2].control = XHCI_TRB_SET_TYPE(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_CYCLE;
        volatile uint32_t* db_slot = (volatile uint32_t*)(ctrl->doorbell_array + 1*4);
        *db_slot = 0;
        kprintf("MSD: BOT RESET issued\n");
    }
    msd_recover_clear_stalls(ctrl, dev);
}

static void msd_issue_read_capacity(xhci_controller_t* ctrl, usb_device_t* dev){
    if(ctrl->msd_op) return;
    usb_msd_cbw_t cbw; for(int i=0;i<16;i++) cbw.cb[i]=0;
    unsigned tag = next_tag(ctrl); ctrl->msd_expected_tag = tag;
    cbw.signature=0x43425355; cbw.tag=tag; cbw.data_len=8; cbw.flags=0x80; cbw.lun=0; cbw.cb_len=10; cbw.cb[0]=SCSI_READ_CAPACITY_10;
    if(!ctrl->msd_data_buf) ctrl->msd_data_buf = kcalloc(1,512);
    if(bot_send_cbw(ctrl, dev, &cbw)==0){
        if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_data_buf, 8)==ST_OK){
            unsigned didx=(ctrl->bulk_in_enqueue-1)%16; ctrl->msd_data_trb=&((xhci_trb_t*)ctrl->bulk_in_ring)[didx]; ctrl->msd_data_phys=(unsigned long)MmGetPhysicalAddress((uint64_t)ctrl->msd_data_trb);
            ctrl->msd_expected_data_len=8; ctrl->msd_state=2; ctrl->msd_op=2; ctrl->msd_need_csw=1; kprintf("MSD: READ CAPACITY (10) issued\n");
        }
    }
}

static int msd_issue_read10(xhci_controller_t* ctrl, usb_device_t* dev, unsigned long lba, unsigned blocks){
    if(ctrl->msd_op || !ctrl->msd_ready) return ST_BUSY;
    unsigned bytes = blocks * ctrl->msd_block_size;
    if(!ctrl->msd_read_buf) ctrl->msd_read_buf = kcalloc(1, bytes);
    if(!ctrl->msd_read_buf) return ST_NOMEM;
    // debug removed: READ10 issue details
    usb_msd_cbw_t cbw; for(int i=0;i<16;i++) cbw.cb[i]=0;
    unsigned tag = next_tag(ctrl); ctrl->msd_expected_tag = tag;
    cbw.signature=0x43425355; cbw.tag=tag; cbw.data_len=bytes; cbw.flags=0x80; cbw.lun=0; cbw.cb_len=10; cbw.cb[0]=0x28; // READ(10)
    cbw.cb[2]=(lba>>24)&0xFF; cbw.cb[3]=(lba>>16)&0xFF; cbw.cb[4]=(lba>>8)&0xFF; cbw.cb[5]=lba&0xFF; cbw.cb[7]=(blocks>>8)&0xFF; cbw.cb[8]=blocks&0xFF;
    if(bot_send_cbw(ctrl, dev, &cbw)==0){
        if(xhci_enqueue_bulk_in(ctrl, dev, ctrl->msd_read_buf, bytes)==ST_OK){
            unsigned didx=(ctrl->bulk_in_enqueue-1)%16; ctrl->msd_data_trb=&((xhci_trb_t*)ctrl->bulk_in_ring)[didx]; ctrl->msd_data_phys=(unsigned long)MmGetPhysicalAddress((uint64_t)ctrl->msd_data_trb);
            ctrl->msd_expected_data_len=bytes; ctrl->msd_state=2; ctrl->msd_op=3; ctrl->msd_need_csw=1;
            ctrl->msd_read_lba=lba; ctrl->msd_read_blocks=blocks; ctrl->msd_read_result=0; return ST_OK;
        }
    }
    return ST_ERR;
}

static int msd_block_read(block_device_t* bdev, unsigned long lba, unsigned long count, void* buf){
    xhci_controller_t* ctrl = (xhci_controller_t*)bdev->driver_data;
    usb_device_t* dev = find_msd(ctrl);
    if(!dev) return ST_INVALID;
    // Debug: show first few block reads
    // debug removed: initial block read trace
    int st = msd_issue_read10(ctrl, dev, lba, count);
    if(st!=ST_OK) return st;
    unsigned long start_poll = ctrl->msd_poll_counter;
    while(ctrl->msd_op==3 && ctrl->msd_state!=4){
        // Service controller events and mass storage state machine cooperatively
        xhci_process_events(ctrl);
        msd_progress(ctrl); // advance BOT if needed
        // Drain any pending keyboard scan codes to keep shell responsive
        extern char keyboard_get_char(void); while(keyboard_get_char()){};
        // Timeout safeguard (arbitrary 10k polls)
        if(ctrl->msd_poll_counter - start_poll > 10000){
            kprintf("MSD: read timeout lba=%lu count=%lu (cbw_ev=%u data_ev=%u csw_ev=%u state=%u op=%u)\n", lba, count,
                ctrl->msd_cbw_events, ctrl->msd_data_events, ctrl->msd_csw_events, ctrl->msd_state, ctrl->msd_op);
            kprintf("  TRB phys: cbw=%p data=%p csw=%p last_evt_ptr=%p last_cc=%u last_epid=%u\n",
                (void*)ctrl->msd_cbw_phys, (void*)ctrl->msd_data_phys, (void*)ctrl->msd_csw_phys, (void*)ctrl->msd_last_event_ptr,
                ctrl->msd_last_event_cc, ctrl->msd_last_event_epid);
            break;
        }
    }
    if(ctrl->msd_read_result <= 0){ /* suppress verbose read fail spam; higher level will handle */ }
    if(ctrl->msd_read_result > 0){
        unsigned copy = (unsigned)ctrl->msd_read_result;
        for(unsigned i=0;i<copy;i++) ((uint8_t*)buf)[i]=((uint8_t*)ctrl->msd_read_buf)[i];
        return ST_OK;
    }
    return ST_ERR;
}

static void msd_progress(xhci_controller_t* ctrl){
    usb_device_t* dev = find_msd(ctrl); if(!dev) return;
    if(!dev->endpoints_configured){
        int cr = xhci_configure_mass_storage_endpoints(ctrl, dev);
    // debug removed: endpoint configure status
        if(cr!=ST_OK) return;
    }
    // Increment poll counter
    ctrl->msd_poll_counter++;
    // Timeout check for active operation
    if(ctrl->msd_op && ctrl->msd_timeout_ticks){
        if(ctrl->msd_poll_counter - ctrl->msd_op_start_tick > ctrl->msd_timeout_ticks){
            kprintf("MSD: Operation %u timeout -> reset\n", ctrl->msd_op);
            msd_recover_reset(ctrl, dev);
            ctrl->msd_state=0; ctrl->msd_op=0; ctrl->msd_timeout_ticks=0;
        }
    }
    if(!ctrl->msd_op){
        if(!ctrl->msd_ready){
            // Pre-ready: run INQUIRY then READ CAPACITY flow, afterwards readiness flag stops further polling
            msd_issue_inquiry(ctrl, dev); ctrl->msd_op_start_tick=ctrl->msd_poll_counter; ctrl->msd_timeout_ticks=5000;
        } else {
            // Once ready, stop issuing periodic TEST UNIT READY to avoid spamming device
            return;
        }
    }
    // (Deferred logic removed; immediate queue of data+CSW used)
    if(ctrl->msd_op==1 && ctrl->msd_state==4){
        char vendor[9]; char product[17]; for(int i=0;i<8;i++){ vendor[i]=((char*)ctrl->msd_data_buf)[8+i]; if(vendor[i]<' '||vendor[i]>'~') vendor[i]=' '; } vendor[8]='\0';
        for(int i=0;i<16;i++){ product[i]=((char*)ctrl->msd_data_buf)[16+i]; if(product[i]<' '||product[i]>'~') product[i]=' '; } product[16]='\0';
        kprintf("MSD: INQUIRY vendor='%s' product='%s'\n", vendor, product);
        ctrl->msd_state=0; ctrl->msd_op=0; msd_issue_read_capacity(ctrl, dev);
        ctrl->msd_op_start_tick=ctrl->msd_poll_counter; ctrl->msd_timeout_ticks=500; // capacity moderate timeout
    }
    if(ctrl->msd_op==2 && ctrl->msd_state==4){
        uint8_t* d=(uint8_t*)ctrl->msd_data_buf; unsigned long last_lba=((unsigned long)d[0]<<24)|((unsigned long)d[1]<<16)|((unsigned long)d[2]<<8)|d[3]; unsigned int blen=(d[4]<<24)|(d[5]<<16)|(d[6]<<8)|d[7];
        ctrl->msd_capacity_blocks=last_lba+1; ctrl->msd_block_size=blen; ctrl->msd_ready=1;
        kprintf("MSD: Capacity blocks=%lu block_size=%u (~%lu KB)\n", ctrl->msd_capacity_blocks, blen, (ctrl->msd_capacity_blocks*(unsigned long)blen)/1024);
        if(!g_msd_block_dev.name){ g_msd_block_dev.name="usb0"; g_msd_block_dev.sector_size=blen; g_msd_block_dev.total_sectors=ctrl->msd_capacity_blocks; g_msd_block_dev.read=msd_block_read; g_msd_block_dev.driver_data=ctrl; if(block_register(&g_msd_block_dev)==ST_OK){ kprintf("MSD: Registered block device 'usb0'\n"); }}
        ctrl->msd_state=0; ctrl->msd_op=0; ctrl->msd_timeout_ticks=0;
    }
    if(ctrl->msd_op==3 && ctrl->msd_state==4){
        usb_msd_csw_t* csw=(usb_msd_csw_t*)ctrl->msd_csw_buf; 
        int tag_ok = (csw->tag == ctrl->msd_expected_tag);
        int sig_ok = (csw->signature == 0x53425355);
        if(!sig_ok || !tag_ok){
            kprintf("MSD: CSW invalid sig_ok=%d tag_ok=%d exp_tag=%u got=%u -> reset\n", sig_ok, tag_ok, ctrl->msd_expected_tag, csw->tag);
            ctrl->msd_read_result=-1; msd_recover_reset(ctrl, dev); ctrl->msd_state=0; ctrl->msd_op=0; return; }
        if(csw->status==0){
            if(csw->residue){ kprintf("MSD: READ(10) residue %u (expected %u)\n", csw->residue, ctrl->msd_expected_data_len); }
            ctrl->msd_read_result=ctrl->msd_expected_data_len - csw->residue;
        } else {
            ctrl->msd_read_result=-1; kprintf("MSD: READ(10) CSW status=%u residue=%u\n", csw->status, csw->residue);
            if(ctrl->msd_retry_count < 3){
                ctrl->msd_retry_count++;
                kprintf("MSD: Retrying READ(10) attempt %u\n", ctrl->msd_retry_count);
                ctrl->msd_state=0; ctrl->msd_op=0; // reissue
                msd_issue_read10(ctrl, dev, ctrl->msd_read_lba, ctrl->msd_read_blocks);
                ctrl->msd_op_start_tick=ctrl->msd_poll_counter; ctrl->msd_timeout_ticks=2000; // data read timeout
                return;
            } else {
                msd_recover_reset(ctrl, dev);
            }
        }
        ctrl->msd_state=0; ctrl->msd_op=0; ctrl->msd_timeout_ticks=0;
    }
    if(ctrl->msd_op==4 && ctrl->msd_state==4){
        usb_msd_csw_t* csw=(usb_msd_csw_t*)ctrl->msd_csw_buf;
        int tag_ok = (csw->tag == ctrl->msd_expected_tag);
        int sig_ok = (csw->signature == 0x53425355);
        if(!sig_ok || !tag_ok){ kprintf("MSD: TUR CSW invalid -> reset\n"); msd_recover_reset(ctrl, dev); ctrl->msd_state=0; ctrl->msd_op=0; return; }
        if(csw->status!=0){
            kprintf("MSD: TEST UNIT READY failed status=%u -> REQUEST SENSE\n", csw->status);
            ctrl->msd_state=0; ctrl->msd_op=0; msd_issue_request_sense(ctrl, dev);
            ctrl->msd_op_start_tick=ctrl->msd_poll_counter; ctrl->msd_timeout_ticks=200; // sense soon
    } else { ctrl->msd_state=0; ctrl->msd_op=0; }
    }
    if(ctrl->msd_op==5 && ctrl->msd_state==4){
        // Parse sense data
        uint8_t* s = (uint8_t*)ctrl->msd_data_buf;
        uint8_t key = s[2] & 0x0F; uint8_t asc = s[12]; uint8_t ascq = s[13];
        ctrl->msd_last_sense_key=key; ctrl->msd_last_sense_asc=asc; ctrl->msd_last_sense_ascq=ascq;
        kprintf("MSD: SENSE key=%u asc=%02x ascq=%02x\n", key, asc, ascq);
        if(key==0x02){ // NOT READY
            if(ctrl->msd_retry_count < 5){ ctrl->msd_retry_count++; kprintf("MSD: Not ready, retry #%u TUR backoff\n", ctrl->msd_retry_count); }
            // exponential backoff: base 500 polls * retry_count
            ctrl->msd_backoff_until = ctrl->msd_poll_counter + (500 * (ctrl->msd_retry_count ? ctrl->msd_retry_count : 1));
        } else if(key==0x06){ // UNIT ATTENTION
            kprintf("MSD: Unit attention, resetting retry counter\n"); ctrl->msd_retry_count=0;
            ctrl->msd_backoff_until = 0;
        } else if(key){
            if(ctrl->msd_reset_count < 2){ ctrl->msd_reset_count++; msd_recover_reset(ctrl, dev); }
        }
        ctrl->msd_state=0; ctrl->msd_op=0; ctrl->msd_timeout_ticks=0;
    }
}

void usb_msd_poll(void* ctrl_ptr){ xhci_controller_t* ctrl=(xhci_controller_t*)ctrl_ptr; msd_progress(ctrl); }

int usb_msd_try_init(void* ctrl_ptr){
    xhci_controller_t* ctrl = (xhci_controller_t*)ctrl_ptr;
    // For now just log if any enumerated device is mass storage with endpoints
    for(int s=1; s<64; ++s){
        usb_device_t* dev = (usb_device_t*)ctrl->slot_device_map[s];
        if(!dev) continue;
        if(dev->class_code == USB_CLASS_MASS_STORAGE && dev->bulk_in_ep && dev->bulk_out_ep){
            kprintf("MSD: Ready (slot %d vid=%04x pid=%04x) - BOT not yet implemented\n", s, dev->vid, dev->pid);
            return 0;
        }
    }
    return -1; // not ready
}
