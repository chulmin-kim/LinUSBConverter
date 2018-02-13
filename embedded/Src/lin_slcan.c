#include "slcan.h"

#include <stdbool.h>
#include <string.h>
#include <sys/_stdint.h>
#include "open_lin_cfg.h"
#include "open_lin_slave_data_layer.h"

#include "usbd_cdc_if.h"
#include "stm32f0xx_hal.h"

uint8_t lin_master_data[MAX_SLAVES_COUNT * 8];
t_master_frame_table_item master_frame_table[MAX_SLAVES_COUNT];
uint8_t master_frame_table_size = 0;

t_master_frame_table_item* slcan_get_master_table_row(open_lin_pid_t id, int8_t* out_index){
	uint8_t i = -1;
	for (i = 0; i < master_frame_table_size; i++)
	{
		if (id == master_frame_table[i].slot.pid)
		{
			break;
		}
	}
	(*out_index) = i;
	return &master_frame_table[i];
}

uint8_t addLinMasterRow(uint8_t* line) {
    uint32_t temp;
    int8_t i,s;
    t_master_frame_table_item* array_ptr = 0;
    uint16_t tFrame_Max_ms;

    // reset schedule table
    if (line[1] == '2')
    {
    	open_lin_hw_reset();
        slcan_state = SLCAN_STATE_CONFIG;
        master_frame_table_size = 0;
        return 1;
    }

    // start sending
    if (line[1] == '1'){
    	open_lin_master_dl_init(master_frame_table,master_frame_table_size);
        slcan_state = SLCAN_STATE_OPEN;
        if (lin_type == LIN_MASTER){
            //wakeUpLin();
        }
        return 1;
    }

    // id
    if (!parseHex(&line[2], 2, &temp)) return 0;
    array_ptr = slcan_get_master_table_row(temp, &s);

    array_ptr->slot.pid= temp;
    // len
    if (!parseHex(&line[4], 1, &temp)) return 0;
    if (array_ptr->slot.data_length  > 8) return 0;
    array_ptr->slot.data_length = temp;

    // type
    if (line[0] == 'r')
    	array_ptr->slot.frame_type = OPEN_LIN_FRAME_TYPE_RECEIVE;
	else
		array_ptr->slot.frame_type = OPEN_LIN_FRAME_TYPE_TRANSMIT;
    // data
    array_ptr->slot.data_ptr = &(lin_master_data[s * 8]); //data is later set in case of override
    // period
    array_ptr->offset_ms = 15;
    // timeout
    tFrame_Max_ms = (((uint16_t)array_ptr->slot.data_length * 10 + 44) * 7 / 100) + 1;
    array_ptr->response_wait_ms = (uint8_t)(tFrame_Max_ms);

    if (array_ptr->slot.frame_type == OPEN_LIN_FRAME_TYPE_TRANSMIT)
    {
        for (i = 0; i < array_ptr->slot.data_length; i++)
        {
            if (!parseHex(&line[5+i*2], 2, &temp)) return 0;
            array_ptr->slot.data_ptr[i] = temp;
        }
    }

    return 1;
}

static t_open_lin_slave_state slcan_lin_slave_state;
static l_u8 slcan_lin_slave_state_data_count;
static uint8_t slcan_lin_data_array[9];
static t_open_lin_data_layer_frame open_lin_data_layer_frame;
uint32_t slcan_lin_timeout_counter = 0;

void lin_slcan_reset(void){
	slcan_lin_slave_state = OPEN_LIN_SLAVE_IDLE;
	slcan_lin_slave_state_data_count = 0;
	slcan_lin_timeout_counter = 0;
}

open_lin_frame_slot_t lin_slcan_slot;
void lin_slcan_rx_handler(t_open_lin_data_layer_frame *f)
{
	lin_slcan_slot.pid = f->pid;
	lin_slcan_slot.data_ptr = f->data_ptr;
	lin_slcan_slot.frame_type = OPEN_LIN_FRAME_TYPE_RECEIVE;
	lin_slcan_slot.data_length = f->lenght;
	slcanReciveCanFrame(&lin_slcan_slot);
}

void lin_slcan_rx_timeout_handler()
{
	open_lin_data_layer_frame.lenght = slcan_lin_slave_state_data_count - 1;
	/* checksum calculation */
	if (slcan_lin_data_array[slcan_lin_slave_state_data_count] == open_lin_data_layer_checksum(open_lin_data_layer_frame.pid,
			open_lin_data_layer_frame.lenght, open_lin_data_layer_frame.data_ptr)) /* TODO remove from interrupt possible function */
	{
		/* valid checksum */
		lin_slcan_rx_handler(&open_lin_data_layer_frame);
	}
	lin_slcan_reset();
}

void lin_slcan_skip_header_reception(uint8_t pid)
{
	lin_slcan_reset();
	slcan_lin_slave_state = OPEN_LIN_SLAVE_DATA_RX;
	open_lin_data_layer_frame.pid = pid;
	open_lin_data_layer_frame.data_ptr = slcan_lin_data_array;
}

void lin_slcan_rx(l_u8 rx_byte)
{

	if (open_lin_hw_check_for_break() == l_true)
	{
		lin_slcan_reset();
		slcan_lin_slave_state = OPEN_LIN_SLAVE_SYNC_RX;
		#ifdef OPEN_LIN_AUTO_BAUND
			open_lin_hw_set_auto_baud();
		#endif
	} else
	{
		switch (slcan_lin_slave_state){
			case (OPEN_LIN_SLAVE_SYNC_RX):
			{
				/* synch byte reception do nothing */
				if (rx_byte != OPEN_LIN_SYNCH_BYTE)
				{
					lin_slcan_reset();
				} else
				{
					slcan_lin_slave_state = OPEN_LIN_SLAVE_PID_RX;
				}
				break;
			}

			case (OPEN_LIN_SLAVE_PID_RX):
			{
				if (open_lin_data_layer_parity(rx_byte) == rx_byte)
				{
					open_lin_data_layer_frame.pid = (open_lin_pid_t)(rx_byte & OPEN_LIN_ID_MASK);
					open_lin_data_layer_frame.data_ptr = slcan_lin_data_array;
				} else
				{
					lin_slcan_reset();
				}
				slcan_lin_slave_state = OPEN_LIN_SLAVE_DATA_RX;
				break;
			}
			case (OPEN_LIN_SLAVE_DATA_RX):
			{
				// slcan_lin_timeout handled in sys timer interrupt function
				slcan_lin_timeout_counter = HAL_GetTick();

				if (slcan_lin_slave_state_data_count < 8)
				{
					open_lin_data_layer_frame.data_ptr[slcan_lin_slave_state_data_count] = rx_byte;
					slcan_lin_slave_state_data_count ++;
				} else
				{
					open_lin_data_layer_frame.lenght = slcan_lin_slave_state_data_count;
					/* checksum calculation */
					if (rx_byte == open_lin_data_layer_checksum(open_lin_data_layer_frame.pid,
							open_lin_data_layer_frame.lenght, open_lin_data_layer_frame.data_ptr)) /* TODO remove from interrupt possible function */
					{
						/* valid checksum */
						lin_slcan_rx_handler(&open_lin_data_layer_frame);
					}
					lin_slcan_reset();
				}
				break;
			}
			default:
				lin_slcan_reset();
				break;
		}
	}
}