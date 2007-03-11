/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Sergio Garcia Murillo <sergio.garcia@fontventa.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Video transcoding
 * 
 * \ingroup applications
 */

#include <asterisk.h>
#include <ffmpeg/avcodec.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/causes.h>

#ifndef AST_FORMAT_AMR
#define AST_FORMAT_AMR		(1 << 13)
#define AST_FORMAT_MPEG4 	(1 << 22)
#endif

struct VideoTranscoder
{
	int end;
	struct ast_channel *channel;

	/* Decoder */
	AVCodec         *decoder;
        AVCodecContext  *decoderCtx;
        AVFrame         *decoderPic;

	char*	pictures[2];
	int	picIndex;
	int	width;
	int 	height;
	int 	newPic;

	char* 	frame;
	int	frameSize;
	int	frameLen;

	/* Encoder */
	AVCodec         *encoder;
        AVCodecContext  *encoderCtx;
        AVFrame         *encoderPic;
	
	char* 	buffer;
	int	bufferSize;
	int	bufferLen;
	int 	mb;
	int	mb_total;
	int 	sent_bytes;

	/* Encoder Params */
	int	bitrate;
	int	fps;
	int	format;
	int	qMin;
	int	qMax;
	int	encoderWidth;
	int 	encoderHeight;
	int 	gop_size;

	/* Encoder thread */
	pthread_t encoderThread;
};

void RtpCallback(struct AVCodecContext *avctx, void *data, int size, int mb_nb);
void * VideoTranscoderEncode(void *param);

static void SendVideoFrame(struct VideoTranscoder *vtc, void *data, int size, int first, int last)
{
	struct ast_frame *send;

	/* Create frame */
	send = (struct ast_frame *) malloc(sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + 2 + size);

	/* clean */
	memset(send,0,sizeof(struct ast_frame) + AST_FRIENDLY_OFFSET + 2 + size);

	/* if it�s first */
	if (first)
	{
		/* No data*/
		send->data = (void*)send + AST_FRIENDLY_OFFSET;
		send->datalen = size;
		/* Copy */
		memcpy(send->data+2, data+2, size-2);
		/* Set header */
		((unsigned char*)(send->data))[0] = 0x04;
		((unsigned char*)(send->data))[1] = 0x00; 
		/* Set timestamp */
		send->samples = 90000/vtc->fps;
	} else {
		/* No data*/
		send->data = (void*)send + AST_FRIENDLY_OFFSET;
		send->datalen =  size + 2  ;
		/* Copy */
		memcpy(send->data+2, data, size);
		/* Set header */
		((unsigned char*)(send->data))[0] = 0x00;
		((unsigned char*)(send->data))[1] = 0x00;
		/* Set timestamp */
		send->samples = 0;
	}

	/* Set video type */
	send->frametype = AST_FRAME_VIDEO;
	/* Set codec value */
	send->subclass = AST_FORMAT_H263_PLUS | last;
	/* Rest of values*/
	send->src = "h324m";
	send->delivery.tv_usec = 0; //(vtc->sent_bytes*8000)/vtc->bitrate;
	send->delivery.tv_sec = 0;
	send->mallocd = 0;

	/* Send */
	vtc->channel->tech->write_video(vtc->channel, send);
}

void * VideoTranscoderEncode(void *param)
{
        struct timeval tv;

	/* Get transcoder context */
	struct VideoTranscoder *vtc = (struct VideoTranscoder*) param;

	/* Until stoped */
	while (!vtc->end)
	{
		/* Calculate sleep time */
		tv.tv_sec  = 0;
		tv.tv_usec = 1000000/vtc->fps;

		/* Sleep */
        	select(0,0,0,0,&tv);

		/* If there are new pic*/
		if (vtc->newPic)
		{
			/* Get buyffer */
			char* buffer =  vtc->pictures[vtc->picIndex];

			/* Change picture decoding index */
			vtc->picIndex = !vtc->picIndex;

			/* Recalc fps */
			//ctx->frame_rate = (int)ctx->fps*ctx->frame_rate_base;

			/* Do we need to resize the image */
			if ( vtc->width!=vtc->encoderWidth || vtc->height!=vtc->encoderHeight)
			{
				printf("[%d,%d,%d,%d]\n", vtc->width,vtc->encoderWidth,vtc->height,vtc->encoderHeight);
				/* Resize frame */
				continue;
			}

			/* Set input picture data */
			int numPixels = vtc->encoderWidth*vtc->encoderHeight;

			/* Set counters */
			vtc->mb = 0;
			vtc->mb_total = ((vtc->encoderWidth+15)/16)*((vtc->encoderHeight+15)/16);
			vtc->sent_bytes = 0;

			/* Set image data */
			vtc->encoderPic->data[0] = buffer;
			vtc->encoderPic->data[1] = buffer+numPixels;
			vtc->encoderPic->data[2] = buffer+numPixels*5/4;
			vtc->encoderPic->linesize[0] = vtc->encoderWidth;
			vtc->encoderPic->linesize[1] = vtc->encoderWidth/2;
			vtc->encoderPic->linesize[2] = vtc->encoderWidth/2;

			/* Encode */
			vtc->bufferLen = avcodec_encode_video(vtc->encoderCtx,vtc->buffer,vtc->bufferSize,vtc->encoderPic);

			int first = 1;
			int last  = 0;
			int sent  = 0;
			int len   = 0;
			
			/* Send */
			while(sent<vtc->bufferLen)
			{
				/* Check remaining */
				if (sent+1400>vtc->bufferLen)
				{
					/* last */
					last = 1;
					/* send the rest */
					len = vtc->bufferLen-sent;
				} else 
					/* Fill */
					len = 1400;

				/*Send packet */
				SendVideoFrame(vtc,vtc->buffer+sent,len,first,last);
				/* Unset first */
				first = 0;
				/* Increment size */
				sent += len;
			}

			/* Reset new pic flag */
			vtc->newPic = 0;
		}
	}

	/* Exit */
	return 0;
		
}

static struct VideoTranscoder * VideoTranscoderCreate(struct ast_channel *channel,char *format)
{
	char *i;

	/* Check params */
	if (strncasecmp(format,"h263",4))
		/* Only h263 output by now*/
		return NULL;

	/* Create transcoder */
	struct VideoTranscoder *vtc = (struct VideoTranscoder *) malloc(sizeof(struct VideoTranscoder));

	/* Set channel */
	vtc->channel	= channel;

	/* Set default parameters */
	vtc->format 	= 0;
	vtc->fps	= -1;
	vtc->bitrate 	= -1;
	vtc->qMin	= -1;
	vtc->qMax	= -1;
	vtc->gop_size	= -1;

	/* Get first parameter */
	i = strchr(format,'@');

	/* Parse param */
	while (i)
	{
		/* skip separator */
		i++;

		/* compare */
		if (strncasecmp(i,"qcif",4)==0)
		{
			/* Set qcif */
			vtc->format = 0;
		} else if (strncasecmp(i,"cif",3)==0) {
			/* Set cif */
			vtc->format = 1;
		} else if (strncasecmp(i,"fps=",4)==0) {
			/* Set fps */
			vtc->fps = atoi(i+4);
		} else if (strncasecmp(i,"kb=",3)==0) {
			/* Set bitrate */
			vtc->bitrate = atoi(i+3)*1024;
		} else if (strncasecmp(i,"qmin=",5)==0) {
			/* Set qMin */
			vtc->qMin = atoi(i+5);
		} else if (strncasecmp(i,"qmax=",5)==0) {
			/* Set qMax */
			vtc->qMax = atoi(i+5);
		} else if (strncasecmp(i,"gs=",3)==0) {
			/* Set gop size */
			vtc->gop_size = atoi(i+3);
		}

		/* Find next param*/
		i = strchr(i,'/');
	}

	printf("-Transcoder [f=%d,fps=%d,kb=%d,qmin=%d,qmax=%d,gs=%d]\n",vtc->format,vtc->fps,vtc->bitrate,vtc->qMin,vtc->qMax,vtc->gop_size);

	/* Depending on the format */
	switch(vtc->format)
	{
		case 0:
			vtc->encoderWidth  = 176;
			vtc->encoderHeight = 144;
			break;
		case 1:
			vtc->encoderWidth  = 352;
			vtc->encoderHeight = 288;
			break;
	}	

	/* Malloc input frame */
	vtc->frameSize	= 65535;
	vtc->frameLen	= 0;
	vtc->frame 	= (char *)malloc(65535);

	/* Malloc output frame */
	vtc->bufferSize	= 65535;
	vtc->bufferLen	= 0;
	vtc->buffer 	= (char *)malloc(65535);

	/* Malloc decodec pictures */
	vtc->pictures[0] = (char *)malloc(1179648); /* Max YUV 1024x768 */
	vtc->pictures[1] = (char *)malloc(1179648); /* 1204*768*1.5 */

	/* First input frame */
	vtc->picIndex	= 0;
	vtc->newPic	= 0;
	vtc->end 	= 0;

	/* Alloc context */
        vtc->decoderCtx = avcodec_alloc_context();
        vtc->encoderCtx = avcodec_alloc_context();

	/* Allocate pictures */
        vtc->decoderPic = avcodec_alloc_frame();
        vtc->encoderPic = avcodec_alloc_frame();

	/* Find encoder */
	vtc->encoder = avcodec_find_encoder(CODEC_ID_H263);
	/* No decoder still */
	vtc->decoder = NULL;

	/* Picture data */
	vtc->encoderCtx->pix_fmt 	= PIX_FMT_YUV420P;
	vtc->encoderCtx->width		= vtc->encoderWidth;
	vtc->encoderCtx->height 	= vtc->encoderHeight;

	/* Rtp mode */
        //vtc->encoderCtx->rtp_mode           = 1;
        //vtc->encoderCtx->rtp_payload_size   = 1400;
        //vtc->encoderCtx->rtp_callback       = RtpCallback;
        //vtc->encoderCtx->opaque             = vtc;

        /* Bitrate */
	if (vtc->bitrate>0)
	{
		/* Set encoder params */
		vtc->encoderCtx->bit_rate           = vtc->bitrate;
        	vtc->encoderCtx->bit_rate_tolerance = 1;
	}

	/* fps*/
	if (vtc->fps>0)
		/* set encoder params*/
        	vtc->encoderCtx->time_base    	    = (AVRational){1,vtc->fps};/* frames per second */

	/* gop size */
	if (vtc->gop_size>0)
		/* set encoder params*/
        	vtc->encoderCtx->gop_size           = vtc->gop_size; // about one Intra frame per second

	/* Bitrate */
	if (vtc->bitrate>0)
	{
		/* set encoder params*/
		vtc->encoderCtx->rc_min_rate        = vtc->bitrate;
		vtc->encoderCtx->rc_max_rate        = vtc->bitrate;
	}	

	/* qMin */
	if (vtc->qMin>0)
		vtc->encoderCtx->mb_qmin = vtc->encoderCtx->qmin= vtc->qMin;
	/* qMax */
	if (vtc->qMax>0)
		vtc->encoderCtx->mb_qmax = vtc->encoderCtx->qmax= vtc->qMax;

        /* Video quality */
        vtc->encoderCtx->rc_buffer_size     = vtc->bufferSize;
        vtc->encoderCtx->rc_qsquish         = 0; //ratecontrol qmin qmax limiting method.
        vtc->encoderCtx->max_b_frames       = 0;
        /*vtc->encoderCtx->i_quant_factor     = (float)-0.6;
        vtc->encoderCtx->i_quant_offset     = (float)0.0;
        vtc->encoderCtx->b_quant_factor     = (float)1.5;*/

        /* Flags */
        vtc->encoderCtx->mb_decision = FF_MB_DECISION_SIMPLE;
        vtc->encoderCtx->flags |= CODEC_FLAG_PASS1;                 //PASS1
        vtc->encoderCtx->flags &= ~CODEC_FLAG_H263P_UMV;            //unrestricted motion vector
        vtc->encoderCtx->flags &= ~CODEC_FLAG_4MV;                  //advanced prediction
        vtc->encoderCtx->flags &= ~CODEC_FLAG_H263P_AIC;            //advanced intra coding*/
        vtc->encoderCtx->flags |= CODEC_FLAG_H263P_SLICE_STRUCT;
	
	/* Open encoder */
	avcodec_open(vtc->encoderCtx, vtc->encoder);

	/* Start encoder thread */
	pthread_create(&vtc->encoderThread,NULL,VideoTranscoderEncode,vtc);

	/* Return encoder */
	return vtc;
}

static int VideoTranscoderDestroy(struct VideoTranscoder *vtc)
{
	/* End encoder */
	vtc->end = 1;

	/* Wait encoder thread to stop */
	pthread_join(vtc->encoderThread,0);

	/* Free pictures */
	free(vtc->pictures[0]);
	free(vtc->pictures[1]);

	/* Free frames */
	free(vtc->frame);
	free(vtc->buffer);

	/* Free decoder */
	if (vtc->decoderCtx)
	{
		/* Close */
		avcodec_close(vtc->decoderCtx);
        	free(vtc->decoderCtx);
	}
	/* Free pic */
        if (vtc->decoderPic)
		free(vtc->decoderPic);

	/* Free encoder */
	if (vtc->encoderCtx)
	{
		/* Close */
		avcodec_close(vtc->encoderCtx);
		free(vtc->encoderCtx);
	}
	/* Free pic */
	if (vtc->encoderPic)
        	free(vtc->encoderPic);

	/* Free */
	free(vtc);

	/* Exit */
	return 1;
}

static void VideoTranscoderCleanFrame(struct VideoTranscoder *vtc)
{
	/* Reset length*/
	vtc->frameLen = 0;
}

static void VideoTranscoderDecodeFrame(struct VideoTranscoder *vtc)
{
	char *bufDecode;
	int got_picture;
	int i;

	/* Decode */
	avcodec_decode_video(vtc->decoderCtx,vtc->decoderPic,&got_picture,vtc->frame,vtc->frameLen);

	/* If it can be decoded */
	if (got_picture)
	{
		/* Check size */
		if(vtc->decoderCtx->width==0 || vtc->decoderCtx->height==0)
			/* Exit */
			return;

		/* Get pointer to frame */
		bufDecode = vtc->pictures[vtc->picIndex];

		/* Save size */
		vtc->width  = vtc->decoderCtx->width;
		vtc->height = vtc->decoderCtx->height;

		/* Get sizes */
		int h = vtc->decoderCtx->height;
		int w = vtc->decoderCtx->width;
		int t = vtc->decoderCtx->width/2;
		int u = w*h;
		int v = w*h*5/4;

		/* Copy Y */
		for(i=0;i<h;i++)
			memcpy(&bufDecode[i*w],&vtc->decoderPic->data[0][i*vtc->decoderPic->linesize[0]],w);

		/* Copy U & V */
		for(i=0;i<h/2;i++)
		{
			memcpy(&bufDecode[i*t+u],&vtc->decoderPic->data[1][i*vtc->decoderPic->linesize[1]],t);
			memcpy(&bufDecode[i*t+v],&vtc->decoderPic->data[2][i*vtc->decoderPic->linesize[2]],t);
		}

		/* Set new frame flag */
		vtc->newPic = 1;
	}
}

static void VideoTranscoderSetDecoder(struct VideoTranscoder *vtc,int codec)
{
	/* If already opened that codec */
	if (vtc->decoder && vtc->decoderCtx->codec_id == codec)
		/* Exit */
		return;

	/* Get decoder */
	vtc->decoder = avcodec_find_decoder(codec);

	/* Clean frame */
	VideoTranscoderCleanFrame(vtc);

        /* Set context parameters*/
        vtc->decoderCtx->workaround_bugs    = 1;
        vtc->decoderCtx->error_concealment  = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
	vtc->decoderCtx->rtp_mode           = 1;
	vtc->decoderCtx->flags |= CODEC_FLAG_PART;

        /* Open */
        avcodec_open(vtc->decoderCtx, vtc->decoder);
}


void RtpCallback(struct AVCodecContext *avctx, void *data, int size, int mb_nb)
{
	/* Get transcoder */
	struct VideoTranscoder *vtc = (struct VideoTranscoder*) avctx->opaque;
	/* Send */
	//SendVideoFrame(vtc,data,size,!vtc->mb,0);
	/* Inc */
	vtc->sent_bytes += size;
	vtc->mb+=mb_nb;
}

static int rfc2190_append(unsigned char *dest,unsigned char *buffer, unsigned bufferLen)
{
	/* Just copy */
	memcpy(dest,buffer,bufferLen);
	/* Return added */
	return bufferLen;
}

static int mpeg4_append(unsigned char *dest,unsigned char *buffer, unsigned bufferLen)
{
	/* Just copy */
	memcpy(dest,buffer,bufferLen);
	/* Return added */
	return bufferLen;
}

static int VideoTranscoderWrite(struct VideoTranscoder *vtc, int codec, unsigned char *buffer, unsigned bufferLen, int mark)
{
	/* If not enougth */
	if (bufferLen + vtc->frameLen > vtc->frameSize);
		/* Clean frame */
		VideoTranscoderCleanFrame(vtc);

	/* Depending on the code */
	if (codec & AST_FORMAT_H263)
	{
		/* Check codec */
		VideoTranscoderSetDecoder(vtc,CODEC_ID_H263);
		/* Depacketize */
		vtc->frameLen += rfc2190_append(vtc->frame+vtc->frameLen,buffer,bufferLen);

	} else if (codec & AST_FORMAT_H263_PLUS) {
		/* Check codec */
		VideoTranscoderSetDecoder(vtc,CODEC_ID_H263);
		/* Depacketize */
		vtc->frameLen += rfc2190_append(vtc->frame+vtc->frameLen,buffer,bufferLen);

	} else if (codec & AST_FORMAT_H264) {
		/* Check codec */
		VideoTranscoderSetDecoder(vtc,CODEC_ID_H264);
		/* Depacketize */
		vtc->frameLen += rfc2190_append(vtc->frame+vtc->frameLen,buffer,bufferLen);

	} else if (codec & AST_FORMAT_MPEG4) {
		/* Check codec */
		VideoTranscoderSetDecoder(vtc,CODEC_ID_MPEG4);
		/* Depacketize */
		vtc->frameLen += mpeg4_append(vtc->frame+vtc->frameLen,buffer,bufferLen);

	}else{
		printf("-Unknown codec [%d]\n",codec);
		return 0;
	}

	/* If mark set */
	if (mark)
		/* Decode frame */
		VideoTranscoderDecodeFrame(vtc);

	/* Clean frame */
	VideoTranscoderCleanFrame(vtc);

	return 1;
}


static int app_transcode(struct ast_channel *chan, void *data)
{
	struct ast_frame *f;
	struct ast_module_user *u;
	int    reason = 0;
	int    ms;
	struct ast_channel *channels[2];
	struct ast_channel *pseudo;
	struct ast_channel *where;
	struct VideoTranscoder *fwd = NULL;
	struct VideoTranscoder *rev = NULL;

	char *fwdParams;
	char *revParams;
	char *local;
	char *a;
	char *b;


	/* Find fwd params */
	if (!(a=strchr((char*)data,'|')))
		return 0;

	/* Find local channel params */
	if (!(b=strchr(a+1,'|')))
		return 0;

	/* Set local params */
	fwdParams = strndup((char*)data,a-(char*)data);
	local 	  = strndup(a+1,b-a-1);
	revParams = strndup(b+1,strlen((char*)data)-(b-(char*)data)-1);

	printf("-transcoding [%s,%s,%s]\n",fwdParams,local,revParams);

	/* Lock module */
	u = ast_module_user_add(chan);


	/* Request new channel */
	pseudo = ast_request("Local", AST_FORMAT_H263 | AST_FORMAT_MPEG4 | AST_FORMAT_H263_PLUS | chan->rawreadformat, local, &reason);
 
	/* If somthing has gone wrong */
	if (!pseudo)
		/* goto end */
		goto end; 

	/* Create contexts */
	fwd = VideoTranscoderCreate(chan,fwdParams);
	rev = VideoTranscoderCreate(chan,revParams);

	/* Free params */
	free(fwdParams);
	free(local);
	free(revParams);


	/* Set caller id */
	ast_set_callerid(pseudo, chan->cid.cid_num, chan->cid.cid_name, chan->cid.cid_num);

	/* Place call */
	if (ast_call(pseudo,data,0))
		/* if fail goto clean */
		goto clean_pseudo;

	/* while not setup */
	while (pseudo->_state!=AST_STATE_UP) {
		/* Wait for data */
		if (ast_waitfor(pseudo, 0)<0)
			/* error, timeout, or done */
			break;
		/* Read frame */
		f = ast_read(pseudo);
		/* If not frame */
		if (!f)
			/* done */ 
			break;
		/* If it's a control frame */
		if (f->frametype == AST_FRAME_CONTROL) {
			/* Dependinf on the event */
			switch (f->subclass) {
				case AST_CONTROL_RINGING:       
					break;
				case AST_CONTROL_BUSY:
				case AST_CONTROL_CONGESTION:
					/* Save cause */
					reason = pseudo->hangupcause;
					/* exit */
					goto hangup_pseudo;
					break;
				case AST_CONTROL_ANSWER:
					/* Set UP*/
					reason = 0;	
					break;
			}
		}
		/* Delete frame */
		ast_frfree(f);
	}

	/* If no answer */
	if (pseudo->_state != AST_STATE_UP)
		/* goto end */
		goto clean_pseudo; 

	/* Set up array */
	channels[0] = chan;
	channels[1] = pseudo;

	/* No timeout */
	ms = -1;

	/* Wait for data avaiable on any channel */
	while (!reason && (where = ast_waitfor_n(channels, 2, &ms)) != NULL) 
	{
		/* Read frame from channel */
		f = ast_read(where);

		/* if it's null */
		if (f == NULL)
			break;

		/* Depending on the channel */
		if (where == chan) 
		{
			/* if it's video */
			if (f->frametype == AST_FRAME_VIDEO) {
				/* If transcode forwdward */
				if (fwd)
				{
					/* Transcode */
					VideoTranscoderWrite(fwd,f->subclass,f->data,f->datalen,f->subclass & 1);
					/* Delete frame */
					ast_frfree(f);
				} else {
					/* Just copy */
					ast_write(pseudo,f);
				}
			} else if (f->frametype == AST_FRAME_CONTROL)  {
				/* Check for hangup */
				if (f->subclass == AST_CONTROL_HANGUP)
					/* Hangup */
					reason = AST_CAUSE_NORMAL_CLEARING;
				/* delete frame */
				ast_frfree(f);
			} else {
				/* Fordward */
				ast_write(pseudo,f);
			}
		} else {
			/* if it's video */
			if (f->frametype == AST_FRAME_VIDEO) {
				/* If transcode backward */
				if (rev)
				{
					/* Transcode */
					VideoTranscoderWrite(rev,f->subclass,f->data,f->datalen,f->subclass & 1);
					/* Delete frame */
					ast_frfree(f);
				} else {
					/* Just copy */
					ast_write(chan,f);
				}
			} else if (f->frametype == AST_FRAME_CONTROL)  {
				/* Check for hangup */
				if (f->subclass == AST_CONTROL_HANGUP)
					/* Hangup */
					reason = AST_CAUSE_NORMAL_CLEARING;
				/* delete frame */
				ast_frfree(f);
			} else {
				/* Fordward */
				ast_write(chan,f);
			}
		}
	}


hangup_pseudo:
	/* Hangup pseudo channel if needed */
	ast_softhangup(pseudo, reason);

clean_pseudo:
	/* Destroy pseudo channel */
	ast_hangup(pseudo);

end:
	/* Destroy transcoders */
	if (fwd)
		VideoTranscoderDestroy(fwd);
	if (rev)
		VideoTranscoderDestroy(rev);

	/* Unlock module*/
	ast_module_user_remove(u);

	/* Without hangup** Without hangup*/
	return 0;
}

static char *name_transcode = "transcode";
static char *syn_transcode = "Video transcode";
static char *des_transcode = "  transcode(informat|channel|outformat):  Estabish connection and transcode video.\n";

static int unload_module(void)
{
	int res;
	res = ast_unregister_application(name_transcode);
	ast_module_user_hangup_all();

	return res;
}

static int load_module(void)
{
	/* Init avcodec */
	avcodec_init();
	
	/* Register all codecs */	
	avcodec_register_all();

	return ast_register_application(name_transcode, app_transcode, syn_transcode, des_transcode);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "H324M stack");
