/******************************************************************************
 * @file           : net.c
 * @brief          : Caregiver link over the on-board ISM43362 Wi-Fi module.
 *
 * Measured: joining ~6.7 s, one send ~10 ms, send + reply ~50 ms. So we join
 * once in net_init() and net_tick() only ever does one send or one quick
 * receive per call.
 *
 * Message format (one line per incident update):
 *   EC1 boot=1A2B3C4D inc=3 seq=7 type=FALL event=ASSIST t=123456
 * boot changes every reset so incident numbers from different runs don't get
 * mixed up; seq lets the receiver spot retries.
 *
 * Replies: "OK seq=n" = receiver got it, "ACK inc=n" = someone pressed
 * Acknowledge on the dashboard.
 ******************************************************************************/

/*--------------------------- Includes ---------------------------------------*/
#include "main.h"
#include "net.h"
#include "incident.h"
#include "log.h"
#include "wifi.h"
#include "wifi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*--------------------------- Configuration ----------------------------------*/
#define NET_SOCKET              1U
#define NET_SEND_TIMEOUT_MS   150U      /* a send normally takes ~10 ms */
#define NET_POLL_TIMEOUT_MS    10U
#define NET_ACK_WAIT_MS      3000U      /* resend if no OK after this */
#define NET_MAX_TRIES           3U
#define NET_RECONNECT_MS     5000U      /* first reconnect delay, doubles each failure */
#define NET_RECONNECT_MAX_MS 30000U
#define NET_KEEPALIVE_MS    10000U      /* send EC1K if nothing else went out */
#define NET_IDLE_POLL_MS      250U      /* how often to check for a late ACK when idle */
#define NET_LINE            128U

/* STM32 unique ID, mixed into boot_id */
#define NET_UID_BASE        0x1FFF7590UL

/*--------------------------- State ------------------------------------------*/

typedef struct
{
    uint32_t          inc_id;
    uint32_t          seq;
    incident_type_t   type;
    incident_status_t status;
    uint32_t          t_ms;
} net_msg_t;

static net_msg_t queue[NET_QUEUE_LEN];
static uint8_t queue_head;      /* next to send */
static uint8_t queue_count;

static uint8_t  online;
static uint32_t boot_id;
static uint32_t next_seq = 1;

/* message waiting for its OK */
static uint8_t  waiting_ack;
static uint32_t sent_at_ms;
static uint32_t sent_seq;
static uint32_t sent_inc_id;
static uint8_t  tries;

static uint32_t next_reconnect_ms;
static uint32_t reconnect_wait_ms = NET_RECONNECT_MS;
static uint32_t last_tx_ms;
static uint32_t last_poll_ms;
static const char *status_text = "not started yet";

/*--------------------------- Helpers ----------------------------------------*/

/* can be called from the ISR (via on_incident_update), so don't print directly */
static void net_say(const char *text)
{
    log_line(text);
}

static const char *type_name(incident_type_t type)
{
    return (type == INC_SOS) ? "SOS" : "FALL";
}

static const char *event_name(incident_status_t status)
{
    switch (status)
    {
    case INC_RESPONSE:  return "RESPONSE";
    case INC_ASSIST:    return "ASSIST";
    case INC_ESCALATED: return "ESCALATE";
    case INC_CANCELLED: return "CANCEL";
    case INC_RESOLVED:  return "RESOLVE";
    default:            return "IDLE";
    }
}

/*--------------------------- The queue --------------------------------------*/

/* The ISR pushes and the main loop pops, so the index updates are done with
 * interrupts off. When full, drop the oldest - each message has the full
 * state so the newest is the one that matters. */
static void queue_push(const net_msg_t *msg)
{
    uint8_t slot;
    uint8_t overflowed = 0;

    __disable_irq();

    if (queue_count == NET_QUEUE_LEN)
    {
        queue_head = (uint8_t)((queue_head + 1U) % NET_QUEUE_LEN);
        queue_count--;
        overflowed = 1;
    }

    slot = (uint8_t)((queue_head + queue_count) % NET_QUEUE_LEN);
    queue_count++;

    __enable_irq();

    queue[slot] = *msg;

    if (overflowed)
    {
        net_say("net: queue full, dropped the oldest update\r\n");
    }
}

static void queue_pop(void)
{
    __disable_irq();

    if (queue_count > 0U)
    {
        queue_head = (uint8_t)((queue_head + 1U) % NET_QUEUE_LEN);
        queue_count--;
    }

    __enable_irq();
}

/* copy of the front message, returns 0 if empty */
static uint8_t queue_peek(net_msg_t *out)
{
    uint8_t got = 0;

    __disable_irq();

    if (queue_count > 0U)
    {
        *out = queue[queue_head];
        got = 1;
    }

    __enable_irq();
    return got;
}

/*--------------------------- Events from incident.c -------------------------*/

/* Called on every incident status change (from the ISR). Only queues. */
void on_incident_update(void)
{
    net_msg_t msg;
    char line[64];

    if (incident_status() == INC_IDLE)
    {
        return;
    }

    msg.inc_id = incident_id();
    msg.seq = next_seq++;
    msg.type = incident_type();
    msg.status = incident_status();
    msg.t_ms = HAL_GetTick();

    queue_push(&msg);

    snprintf(line, sizeof(line), "net: queued seq=%lu %s %s (%u waiting)\r\n",
             (unsigned long)msg.seq, type_name(msg.type),
             event_name(msg.status), (unsigned)queue_count);
    net_say(line);
}

/*--------------------------- Interrupt glue ---------------------------------*/

/* Wi-Fi data-ready on PE1 (EXTI1_IRQHandler -> HAL -> here). Without this the
 * driver hangs waiting for the module. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_1)
    {
        SPI_WIFI_ISR();
    }
}

/*--------------------------- Talking to the laptop --------------------------*/

static uint8_t open_connection(void)
{
    uint8_t ip[4] = RECEIVER_IP;

    return (WIFI_OpenClientConnection(NET_SOCKET, WIFI_TCP_PROTOCOL, "wearable",
                                      ip, RECEIVER_PORT, WEARABLE_PORT)
            == WIFI_STATUS_OK) ? 1U : 0U;
}

/* Mark the link down. Whatever was in flight is still in the queue. */
static void link_lost(const char *why)
{
    char line[64];

    if (online)
    {
        snprintf(line, sizeof(line), "net: link lost (%s)\r\n", why);
        net_say(line);
    }

    online = 0;
    waiting_ack = 0;
    reconnect_wait_ms = NET_RECONNECT_MS;
    next_reconnect_ms = HAL_GetTick() + NET_RECONNECT_MS;
    (void)WIFI_CloseClientConnection(NET_SOCKET);
}

/* Send the front of the queue (stays queued until the OK comes back) */
static void send_front(uint32_t now)
{
    net_msg_t message;
    const net_msg_t *msg = &message;
    char body[NET_LINE];
    uint16_t sent = 0;
    int length;

    if (!queue_peek(&message))
    {
        return;
    }

    length = snprintf(body, sizeof(body),
                      "EC1 boot=%08lX inc=%lu seq=%lu type=%s event=%s t=%lu\n",
                      (unsigned long)boot_id,
                      (unsigned long)msg->inc_id,
                      (unsigned long)msg->seq,
                      type_name(msg->type),
                      event_name(msg->status),
                      (unsigned long)msg->t_ms);

    if ((length <= 0) || ((size_t)length >= sizeof(body)))
    {
        queue_pop();        /* shouldn't happen, but don't get stuck on it */
        return;
    }

    if (WIFI_SendData(NET_SOCKET, (uint8_t *)body, (uint16_t)length, &sent,
                      NET_SEND_TIMEOUT_MS) != WIFI_STATUS_OK)
    {
        link_lost("send failed");
        return;
    }

    {
        char note[64];

        snprintf(note, sizeof(note), "net: sent seq=%lu (%u bytes)\r\n",
                 (unsigned long)msg->seq, (unsigned)sent);
        net_say(note);
    }

    /* sent but not confirmed yet */
    incident_delivery(msg->inc_id, MSG_PENDING);
    last_tx_ms = now;
    sent_inc_id = msg->inc_id;
    waiting_ack = 1;
    sent_at_ms = now;
    sent_seq = msg->seq;
    tries++;
}

/* Check for "OK seq=" / "ACK inc=" without blocking */
static void poll_reply(uint32_t now)
{
    char reply[NET_LINE];
    uint16_t got = 0;
    char *found;

    if (WIFI_ReceiveData(NET_SOCKET, (uint8_t *)reply, sizeof(reply) - 1U,
                         &got, NET_POLL_TIMEOUT_MS) != WIFI_STATUS_OK)
    {
        /* timeout with nothing received is normal */
        got = 0;
    }

    if (got > 0U)
    {
        reply[got] = '\0';

        found = strstr(reply, "OK seq=");
        if ((found != NULL) && waiting_ack)
        {
            uint32_t seq = (uint32_t)strtoul(found + 7, NULL, 10);

            if (seq == sent_seq)
            {
                incident_delivery(sent_inc_id, MSG_DELIVERED);
                queue_pop();
                waiting_ack = 0;
                tries = 0;
            }
        }

        found = strstr(reply, "ACK inc=");
        if (found != NULL)
        {
            uint32_t id = (uint32_t)strtoul(found + 8, NULL, 10);
            char line[64];

            /* incident.c ignores it if it's for an old incident */
            incident_delivery(id, MSG_ACKED);
            snprintf(line, sizeof(line), "net: caregiver acknowledged inc=%lu\r\n",
                     (unsigned long)id);
            net_say(line);
        }
    }

    /* no OK in time -> resend (send_front picks it up again), give up after NET_MAX_TRIES */
    if (waiting_ack && ((now - sent_at_ms) >= NET_ACK_WAIT_MS))
    {
        waiting_ack = 0;

        if (tries >= NET_MAX_TRIES)
        {
            tries = 0;
            link_lost("no answer");
        }
    }
}

/*--------------------------- The tick ---------------------------------------*/

static void link_back(uint32_t now)
{
    online = 1;
    waiting_ack = 0;
    tries = 0;
    reconnect_wait_ms = NET_RECONNECT_MS;
    last_tx_ms = now;
    net_say("net: link back up\r\n");
}

/* First try just reopening the socket. If that fails the hotspot probably
 * went away, so rejoin the network (slow - this blocks the main loop for a
 * few seconds, detection keeps running in the ISR). Back-off 5 s -> 30 s. */
static void try_reconnect(uint32_t now)
{
    next_reconnect_ms = now + reconnect_wait_ms;
    reconnect_wait_ms *= 2U;
    if (reconnect_wait_ms > NET_RECONNECT_MAX_MS)
    {
        reconnect_wait_ms = NET_RECONNECT_MAX_MS;
    }

    if (open_connection())
    {
        link_back(now);
        return;
    }

    /* the module won't rejoin a network it thinks it's still on */
    net_say("net: rejoining the network\r\n");
    (void)WIFI_Disconnect();

    if (WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK)
    {
        return;
    }

    if (open_connection())
    {
        link_back(now);
    }
}

/* Without this we wouldn't notice the hotspot going away while the queue is
 * empty. The receiver doesn't treat EC1K as an incident. */
static void send_keepalive(uint32_t now)
{
    char body[NET_LINE];
    uint16_t sent = 0;
    int length;

    length = snprintf(body, sizeof(body), "EC1K boot=%08lX t=%lu\n",
                      (unsigned long)boot_id, (unsigned long)now);

    if ((length > 0) && ((size_t)length < sizeof(body)))
    {
        if (WIFI_SendData(NET_SOCKET, (uint8_t *)body, (uint16_t)length, &sent,
                          NET_SEND_TIMEOUT_MS) != WIFI_STATUS_OK)
        {
            link_lost("no answer to keepalive");
            return;
        }
    }

    last_tx_ms = now;
}

void net_tick(void)
{
    uint32_t now = HAL_GetTick();

    if (!online)
    {
        if ((int32_t)(now - next_reconnect_ms) >= 0)
        {
            try_reconnect(now);
        }
        return;
    }

    /* one thing per call: wait for the OK, or send the next message */
    if (waiting_ack)
    {
        poll_reply(now);
    }
    else if (queue_count > 0U)
    {
        send_front(now);
    }
    else
    {
        /* idle - still check for ACKs now and then, but not every 20 ms */
        if ((now - last_poll_ms) >= NET_IDLE_POLL_MS)
        {
            last_poll_ms = now;
            poll_reply(now);
        }

        if ((now - last_tx_ms) >= NET_KEEPALIVE_MS)
        {
            send_keepalive(now);
        }
    }
}

/*--------------------------- Read-only access -------------------------------*/

uint8_t net_is_online(void)     { return online; }
uint8_t net_queue_depth(void)   { return queue_count; }
const char *net_status_text(void) { return status_text; }

/*--------------------------- Start-up ---------------------------------------*/

/* After reflashing, the radio keeps power and still holds the old connection,
 * and then fails to start (unplugging USB fixed it). The driver's reset pulse
 * is too short, so do a long one on PE8 ourselves. */
static void radio_hard_reset(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* CS (PE0) floats after an MCU reset and the module can take that as a
     * transfer in progress, so drive it high first. Wake-up (PB13) low. */
    gpio.Pin = GPIO_PIN_0;                      /* CS */
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &gpio);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_0, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_13;                     /* wake-up */
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);

    gpio.Pin = ISM43362_RST_Pin;
    HAL_GPIO_Init(ISM43362_RST_GPIO_Port, &gpio);

    HAL_GPIO_WritePin(ISM43362_RST_GPIO_Port, ISM43362_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(300);
    HAL_GPIO_WritePin(ISM43362_RST_GPIO_Port, ISM43362_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(1000);    /* module boot time */
}

void net_init(void)
{
    const uint32_t *uid = (const uint32_t *)NET_UID_BASE;
    char line[80];
    uint8_t own_ip[4] = {0, 0, 0, 0};

    queue_head = 0;
    queue_count = 0;
    online = 0;
    waiting_ack = 0;
    tries = 0;
    next_reconnect_ms = 0;
    reconnect_wait_ms = NET_RECONNECT_MS;
    last_tx_ms = HAL_GetTick();

    /* unique ID xor the boot time - different every run */
    boot_id = uid[0] ^ uid[1] ^ uid[2] ^ HAL_GetTick();

    radio_hard_reset();

    if (WIFI_Init() != WIFI_STATUS_OK)
    {
        net_say("net: radio did not start; resetting it and trying once more\r\n");
        radio_hard_reset();

        if (WIFI_Init() != WIFI_STATUS_OK)
        {
            status_text = "Radio did not start";
            net_say("net: no radio; running offline\r\n");
            return;
        }
    }

    /* after a warm restart the module may still be joined / holding the old
     * socket and will refuse to join again, so release both first */
    (void)WIFI_CloseClientConnection(NET_SOCKET);
    (void)WIFI_Disconnect();

    if (WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK)
    {
        net_say("net: join refused; letting go and trying once more\r\n");
        (void)WIFI_Disconnect();
        HAL_Delay(500);

        if (WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK) != WIFI_STATUS_OK)
        {
            status_text = "Join refused";
            net_say("net: could not join the network; running offline\r\n");
            return;
        }
    }

    if (WIFI_GetIP_Address(own_ip) == WIFI_STATUS_OK)
    {
        snprintf(line, sizeof(line), "net: joined as %u.%u.%u.%u\r\n",
                 own_ip[0], own_ip[1], own_ip[2], own_ip[3]);
        net_say(line);
    }

    if (open_connection())
    {
        online = 1;
        status_text = "Link ok";
        snprintf(line, sizeof(line), "net: link up, boot id %08lX\r\n",
                 (unsigned long)boot_id);
        net_say(line);
    }
    else
    {
        status_text = "No receiver on PC";
        net_say("net: no receiver; messages will wait in the queue\r\n");
        next_reconnect_ms = HAL_GetTick() + NET_RECONNECT_MS;
    }
}
