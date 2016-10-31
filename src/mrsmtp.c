/*******************************************************************************
 *
 *                             Messenger Backend
 *     Copyright (C) 2016 Björn Petersen Software Design and Development
 *                   Contact: r10s@b44t.com, http://b44t.com
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see http://www.gnu.org/licenses/ .
 *
 *******************************************************************************
 *
 * File:    mrsmtp.c
 * Authors: Björn Petersen
 * Purpose: Use SMTP servers
 *
 ******************************************************************************/


#include <stdlib.h>
#include <string.h>
#include <libetpan/libetpan.h>
#include "mrmailbox.h"
#include "mrsmtp.h"
#include "mrlog.h"
#include "mrtools.h"

#ifndef DEBUG_SMTP
#define DEBUG_SMTP 0
#endif


/*******************************************************************************
 * Main interface
 ******************************************************************************/


mrsmtp_t* mrsmtp_new(void)
{
	mrsmtp_t* ths;
	if( (ths=calloc(1, sizeof(mrsmtp_t)))==NULL ) {
		exit(29);
	}
	return ths;
}


void mrsmtp_unref(mrsmtp_t* ths)
{
	if( ths == NULL ) {
		return;
	}
	mrsmtp_disconnect(ths);
	free(ths->m_from);
	free(ths);
}


/*******************************************************************************
 * Connect/Disconnect
 ******************************************************************************/


int mrsmtp_is_connected (mrsmtp_t* ths)
{
	return (ths && ths->m_hEtpan)? 1 : 0;
}


static void body_progress(size_t current, size_t maximum, void* user_data)
{
	mrlog_info("body_progress called with current=%i, maximum=%i.", (int)current, (int)maximum);
}


#if DEBUG_SMTP
static void logger(mailsmtp* smtp, int log_type, const char* buffer__, size_t size, void* user_data)
{
	char* buffer = malloc(size+1);
	memcpy(buffer, buffer__, size);
	buffer[size] = 0;
	mrlog_info("SMPT: %i: %s", log_type, buffer__);
}
#endif


int mrsmtp_connect(mrsmtp_t* ths, const mrloginparam_t* lp)
{
	int         success = 0;
	int         smtp_esmtp = 1, smtp_ssl = 1, smtp_tls = 0; /* TODO: make additional configurations available */
	int         ret;

	if( ths == NULL ) {
		return 0;
	}

	if( ths->m_hEtpan ) {
		mrlog_warning("Already connected to SMTP server.");
		return 1;
	}

	if( lp->m_addr == NULL || lp->m_send_server == NULL || lp->m_send_port == 0 ) {
		mrlog_error("Cannot connect to SMTP; bad parameters.");
		return 0;
	}

	free(ths->m_from);
	ths->m_from = safe_strdup(lp->m_addr);

	ths->m_hEtpan = mailsmtp_new(0, NULL);
	if( ths->m_hEtpan == NULL ) {
		mrlog_error("mailsmtp_new() failed.");
	}
	mailsmtp_set_progress_callback(ths->m_hEtpan, body_progress, ths);
	#if DEBUG_SMTP
		mailsmtp_set_logger(ths->m_hEtpan, logger, ths);
	#endif

	/* first open the stream */
	if( smtp_ssl ) {
		/* use SMTP over SSL */
		if( (ret=mailsmtp_ssl_connect(ths->m_hEtpan, lp->m_send_server, lp->m_send_port)) != MAILSMTP_NO_ERROR ) {
			mrlog_error("mailsmtp_ssl_connect: %s\n", mailsmtp_strerror(ret));
			goto cleanup;
		}
	}
	else {
		/* use STARTTLS */
		if( (ret=mailsmtp_socket_connect(ths->m_hEtpan, lp->m_send_server, lp->m_send_port)) != MAILSMTP_NO_ERROR ) {
			mrlog_error("mailsmtp_socket_connect: %s\n", mailsmtp_strerror(ret));
			goto cleanup;
		}
	}

	/* then introduce ourselves */
	ths->m_esmtp = 0;
	if (smtp_esmtp && (ret = mailesmtp_ehlo(ths->m_hEtpan)) == MAILSMTP_NO_ERROR) {
		ths->m_esmtp = 1;
	}
	else if (!smtp_esmtp || ret == MAILSMTP_ERROR_NOT_IMPLEMENTED) {
		ret = mailsmtp_helo(ths->m_hEtpan);
	}

	if (ret != MAILSMTP_NO_ERROR) {
		mrlog_error("mailsmtp_helo: %s\n", mailsmtp_strerror(ret));
		goto cleanup;
	}

	if( ths->m_esmtp && smtp_tls && (ret=mailsmtp_socket_starttls(ths->m_hEtpan)) != MAILSMTP_NO_ERROR ) {
		mrlog_error("mailsmtp_starttls: %s\n", mailsmtp_strerror(ret));
		goto cleanup;
	}

	if (ths->m_esmtp && smtp_tls) {
		/* introduce ourselves again */
		if (smtp_esmtp && (ret = mailesmtp_ehlo(ths->m_hEtpan)) == MAILSMTP_NO_ERROR) {
			ths->m_esmtp = 1;
		}
		else if (!smtp_esmtp || ret == MAILSMTP_ERROR_NOT_IMPLEMENTED) {
			ret = mailsmtp_helo(ths->m_hEtpan);
		}

		if (ret != MAILSMTP_NO_ERROR) {
			mrlog_error("mailsmtp_helo: %s\n", mailsmtp_strerror(ret));
			goto cleanup;
		}
	}

	if (ths->m_esmtp
	 && lp->m_send_user!=NULL
	 && (ret=mailsmtp_auth(ths->m_hEtpan, lp->m_send_user, lp->m_send_pw))!=MAILSMTP_NO_ERROR ) {
		mrlog_error("mailsmtp_auth: %s: %s\n", lp->m_send_user, mailsmtp_strerror(ret));
		goto cleanup;
	}

	/* cleanup */
	success = 1;

cleanup:
	if( !success ) {
		mailsmtp_free(ths->m_hEtpan);
		ths->m_hEtpan = NULL;
	}
	return success;
}


void mrsmtp_disconnect(mrsmtp_t* ths)
{
	if( ths == NULL ) {
		return;
	}

	if( ths->m_hEtpan ) {
		//mailsmtp_quit(ths->m_hEtpan); -- ?
		mailsmtp_free(ths->m_hEtpan);
		ths->m_hEtpan = NULL;
	}
}


/*******************************************************************************
 * Send a message
 ******************************************************************************/


int mrsmtp_send_msg(mrsmtp_t* ths, const clist* recipients, const char* data)
{
	int           success = 0, ret;
	clistiter*    iter;

	if( ths == NULL ) {
		return 0;
	}

	if( recipients == NULL || clist_count(recipients)==0 || data == NULL ) {
		return 1; /* "null message" send */
	}

	/* set source */
	if( (ret=(ths->m_esmtp?
			mailesmtp_mail(ths->m_hEtpan, ths->m_from, 1, "etPanSMTPTest") :
	         mailsmtp_mail(ths->m_hEtpan, ths->m_from))) != MAILSMTP_NO_ERROR ) {
		mrlog_error("mailsmtp_mail: %s, %s\n", ths->m_from, mailsmtp_strerror(ret));
		goto cleanup;
	}

	/* set recipients */
	for( iter=clist_begin(recipients); iter!=NULL; iter=clist_next(iter)) {
		const char* rcpt = clist_content(iter);
		if( (ret = (ths->m_esmtp?
				 mailesmtp_rcpt(ths->m_hEtpan, rcpt, MAILSMTP_DSN_NOTIFY_FAILURE|MAILSMTP_DSN_NOTIFY_DELAY, NULL) :
				  mailsmtp_rcpt(ths->m_hEtpan, rcpt))) != MAILSMTP_NO_ERROR) {
			mrlog_error("mailsmtp_rcpt: %s: %s\n", rcpt, mailsmtp_strerror(ret));
			goto cleanup;
		}
	}


	/* message */
	if ((ret = mailsmtp_data(ths->m_hEtpan)) != MAILSMTP_NO_ERROR) {
		fprintf(stderr, "mailsmtp_data: %s\n", mailsmtp_strerror(ret));
		goto cleanup;
	}

	if ((ret = mailsmtp_data_message(ths->m_hEtpan, data, strlen(data))) != MAILSMTP_NO_ERROR) {
		fprintf(stderr, "mailsmtp_data_message: %s\n", mailsmtp_strerror(ret));
		goto cleanup;
	}

	/* done */
	success = 1;

cleanup:
	return success;
}

