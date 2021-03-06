#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "telehash.h"

// simple packet list
typedef struct qlob_s {
  struct qlob_s *next;
  lob_t pkt;
} qlob_s, *qlob_t;

struct util_frames_s {
  uint32_t magic;
  uint32_t max;

  // inlen is expected, inat is partial progress
  uint8_t *in;
  uint32_t inlen, inat;

  // either header or lob_raw(outbox)
  uint8_t *out;
  uint32_t outlen;

  // internal queues for ISR safety
  qlob_t inbox; // owned by inbox(), locked by receive()
  qlob_t outbox; // owned by outbox(), locked by send()

  uint8_t is_receiving : 1;
  uint8_t is_sending : 1;
  uint8_t inbox_err : 1;
};

qlob_t qlob_append(qlob_t q, lob_t lob)
{
  if(!q)
  {
    q = malloc(sizeof(qlob_s));
    q->next = NULL;
    q->pkt = lob;
  } else if(!q->next) {
    q->next = qlob_append(NULL, lob);
  }else{
    qlob_append(q->next, lob);
  }
  return q;
}

qlob_t qlob_free(qlob_t q)
{
  if(!q) return NULL;
  qlob_t next = q->next;
  lob_free(q->pkt);
  free(q);
  return qlob_free(next);
}

util_frames_t util_frames_new(uint32_t magic, uint32_t max)
{
  util_frames_t frames;
  if(!(frames = malloc(sizeof(struct util_frames_s)))) return LOG_WARN("OOM");
  memset(frames, 0, sizeof(struct util_frames_s));
  frames->max = max;
  frames->magic = magic;

  return frames;
}

util_frames_t util_frames_free(util_frames_t frames)
{
  if(!frames) return NULL;
  qlob_free(frames->inbox);
  qlob_free(frames->outbox);
  if(frames->inlen == 8) free(frames->in);
  if(frames->outlen == 8) free(frames->out);
  free(frames);
  return NULL;
}

util_frames_t util_frames_clear(util_frames_t frames)
{
  if(!frames) return NULL;
  if(frames->inlen == 8) free(frames->in);
  frames->in = NULL;
  frames->inlen = frames->inat = 0;
  frames->inbox_err = false;

// cannot clear out as it may be active
//  if(frames->outlen == 8) free(frames->out);
//  frames->out = NULL;
//  frames->outlen = 0;
  return frames;
}

util_frames_t util_frames_ok(util_frames_t frames)
{
  return (frames && !frames->inbox_err)?frames:NULL;
}

// turn this packet into frames and append, takes ownership of out
util_frames_t util_frames_send(util_frames_t frames, lob_t out)
{
  if(!frames) return LOG_WARN("bad args");
  if(lob_len(out) < 8)
  {
    LOG_WARN("out packet too small: %s",util_hex(lob_raw(out),lob_len(out),NULL));
    lob_free(out);
    return NULL;
  }

  frames->is_sending = true;
  frames->outbox = qlob_append(frames->outbox, out);
  frames->is_sending = false;
  return frames;
}

// get any packets that have been reassembled from incoming frames
lob_t util_frames_receive(util_frames_t frames)
{
  if(!frames) return LOG_WARN("bad args");
  frames->is_receiving = true;
  lob_t ret = NULL;
  for(qlob_t q = frames->inbox;q;q = q->next) if(q->pkt)
  {
    ret = q->pkt;
    q->pkt = NULL;
    break;
  }
  frames->is_receiving = false;
  return ret;
}

// total bytes in the inbox/outbox
uint32_t util_frames_inlen(util_frames_t frames)
{
  if(!frames) return 0;
  uint32_t total = 0;
  for(qlob_t q = frames->inbox; q; q = q->next) total += lob_len(q->pkt);
  return total;
}

uint32_t util_frames_outlen(util_frames_t frames)
{
  if(!frames) return 0;
  uint32_t total = 0;
  for(qlob_t q = frames->outbox; q; q = q->next) total += lob_len(q->pkt);
  return total;
}

util_frames_t util_frames_pending(util_frames_t frames)
{
  return (util_frames_outlen(frames))?frames:NULL;
}

uint8_t *util_frames_awaiting(util_frames_t frames, uint32_t *len)
{
  if(!frames) return LOG_WARN("bad args");

  // prepare for header if none
  if(!frames->inlen) {
    frames->in = malloc(8);
    frames->inlen = 8;
    frames->inat = 0;
  }

  if(len) *len = frames->inlen - frames->inat;
  return frames->in+frames->inat;
}

// data may be pointer returned from awaiting for zero-copy usage
util_frames_t util_frames_inbox(util_frames_t frames, uint8_t *data, uint32_t len)
{
  if(!frames || !data || !len) return LOG_WARN("bad args");

  // init to header if none
  if(!frames->inlen) util_frames_awaiting(frames, NULL);

  uint32_t await = frames->inlen - frames->inat;

  // take partial and advance
  if(len < await){
    if(data != (frames->in + frames->inat)) memcpy(frames->in + frames->inat, data, len);
    frames->inat += len;
    return frames;
  }

  // if there's a remainder, stash it
  uint8_t *more = NULL;
  uint32_t morelen = 0;
  if(len > await)
  {
    more = data + await;
    morelen = len - await;
    len = await;
  }

  // handle complete frame now
  if(data != (frames->in + frames->inat)) memcpy(frames->in + frames->inat, data, len);

  // is it a header
  if(frames->inlen == 8)
  {
    uint32_t inmagic;
    uint32_t inlen;
    memcpy(&(inmagic),data,4);
    memcpy(&(inlen),data+4,4);
    free(frames->in);
    frames->in = NULL;
    frames->inlen = frames->inat = 0;
    // ensure correct
    if(inmagic != frames->magic || inlen > frames->max)
    {
      frames->inbox_err = true;
      return LOG_INFO("magic/length header mismatch: %lu/%lu %lu<%lu",frames->magic,inmagic,inlen,frames->max);
    }
    frames->inbox_err = false;
    // make space for full packet frame
    if(!(frames->in = malloc(inlen))) return LOG_WARN("OOM");
    frames->inlen = inlen;
  }else{
    // free any leading inbox empties
    qlob_t q = NULL;
    while(!frames->is_receiving && (q = frames->inbox) && !q->pkt) {
      frames->inbox = q->next;
      q->next = NULL;
      qlob_free(q);
    }

    // new inbox packet yay
    LOG_DEBUG("new pkt len %lu",frames->inlen);
    frames->inbox = qlob_append(frames->inbox, lob_direct(frames->in,frames->inlen));
    frames->in = NULL;
    frames->inlen = frames->inat = 0;
  }

  return (more)?util_frames_inbox(frames,more,morelen):frames;
}

uint8_t *util_frames_outbox(util_frames_t frames, uint32_t *len)
{
  if(!len) return LOG_WARN("invalid usage");
  *len = 0;
  if(!frames) return LOG_WARN("bad args");

  // load up next frame if none
  if(!frames->outlen)
  {
    // free any leading empties
    qlob_t q = NULL;
    while(!frames->is_sending && (q = frames->outbox) && !q->pkt) {
      frames->outbox = q->next;
      q->next = NULL;
      qlob_free(q);
    }
    if(!frames->outbox) return NULL; // empty

    // add header for next pkt
    LOG_DEBUG("sending header");
    frames->out = malloc(8);
    frames->outlen = 8;
    memcpy(frames->out,&(frames->magic),4);
    uint32_t len = lob_len(frames->outbox->pkt);
    memcpy(frames->out+4, &(len), 4);
  }

  *len = frames->outlen;
  return frames->out;
}

util_frames_t util_frames_sent(util_frames_t frames)
{
  if(!frames) return LOG_WARN("bad args");
  if(!frames->outlen || !frames->outbox) return LOG_WARN("invalid usage");

  // check if header was just sent
  if(frames->outlen == 8) {
    free(frames->out);
    frames->out = lob_raw(frames->outbox->pkt);
    frames->outlen = lob_len(frames->outbox->pkt);
  }else{
    // packet is sent
    frames->out = NULL;
    frames->outlen = 0;
    frames->outbox->pkt = lob_free(frames->outbox->pkt);
  }

  // return if there's more to go
  return util_frames_outlen(frames)?frames:NULL;
}

// are we active to sending/receiving frames
util_frames_t util_frames_busy(util_frames_t frames)
{
  return ((util_frames_inlen(frames) + util_frames_outlen(frames)) > 0)?frames:NULL;
}


