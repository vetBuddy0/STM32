/******************************************************************************
 * @file           : incident.h
 * @brief          : Detector states and the incident interface.
 *
 * fall_state_t is what the detector thinks right now (changes a lot).
 * incident_status_t is how far the current incident has got (response
 * window -> assist -> escalated -> closed). They're separate on purpose: an
 * SOS creates an incident while the detector is still in NORMAL.
 ******************************************************************************/
#ifndef INCIDENT_H
#define INCIDENT_H

#include <stdint.h>

/*--------------------------- The detector -----------------------------------*/

/* Order matters, used to index STATE_NAMES in main.c */
typedef enum
{
    STATE_NORMAL = 0,   /* ordinary activity */
    STATE_SUSPECT,      /* dip or fast rotation seen */
    STATE_IMPACT,       /* hit seen, waiting for still + new pose */
    STATE_FALL          /* confirmed, latched */
} fall_state_t;

/* Detector's record of the latest fall */
typedef struct
{
    uint32_t     id;          /* +1 per confirmed fall */
    fall_state_t state;
    uint32_t     t_start_ms;  /* tick when confirmed */
} incident_t;

/*--------------------------- The incident -----------------------------------*/

typedef enum
{
    INC_NONE = 0,
    INC_FALL,           /* from the detector */
    INC_SOS             /* button held 3 s */
} incident_type_t;

typedef enum
{
    INC_IDLE = 0,
    INC_RESPONSE,       /* alarm on, wearer can press "I'm OK" */
    INC_ASSIST,         /* help requested */
    INC_ESCALATED,      /* long lie */
    INC_CANCELLED,      /* wearer cancelled */
    INC_RESOLVED        /* wearer got back up */
} incident_status_t;

/* Message state. Delivered (receiver got it) and acked (a person clicked
 * Acknowledge) are kept separate. */
typedef enum
{
    MSG_NONE = 0,
    MSG_PENDING,
    MSG_DELIVERED,
    MSG_ACKED           /* does not close the incident */
} delivery_t;

/*--------------------------- Module interface -------------------------------*/

/* also sets up the button and buzzer/LED */
void incident_init(void);

/* Called every detection step.
 * upright: back within POSE_RETURN_DEG of the pre-fall pose
 * still:   gyro below STILL_DPS */
void incident_tick(uint8_t upright, uint8_t still);

/* Button actions */
void incident_request_sos(void);
void incident_wearer_ok(void);      /* only does something in INC_RESPONSE */
void incident_cancel(void);

/* Called by net.c when an OK / ACK comes back */
void incident_delivery(uint32_t id, delivery_t state);

/* Getters for screens.c and net.c */
uint32_t          incident_id(void);
incident_type_t   incident_type(void);
incident_status_t incident_status(void);
delivery_t        incident_delivery_state(void);
uint8_t           incident_is_active(void);
uint32_t          incident_age_ms(void);        /* since the incident started */
uint32_t          incident_phase_ms(void);      /* since the last status change */
uint32_t          incident_response_left_ms(void);
uint8_t           incident_wearer_still(void);
uint8_t           incident_demo_timers(void);   /* 1 = shortened demo timers */

/*--------------------------- Hooks ------------------------------------------*/

/* Called by main.c on entering / leaving FALL. Defined in incident.c */
void on_fall_confirmed(const incident_t *incident);
void on_recovered(const incident_t *incident);

/* Set by incident.c on cancel, main.c checks it in STATE_FALL and clears it.
 * Also cleared on entering FALL so an earlier press doesn't count. */
extern volatile uint8_t fall_cancel_request;

/* Called on every status change, net.c uses it to queue a message */
void on_incident_update(void);

#endif /* INCIDENT_H */
