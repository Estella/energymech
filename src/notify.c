/*

    EnergyMech, IRC bot software
    Copyright (c) 2001-2004 proton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/
#define NOTIFY_C
#include "config.h"

#ifdef NOTIFY

#include "defines.h"
#include "structs.h"
#include "global.h"
#include "h.h"
#include "text.h"

#define CHOOSE_NUMBER	25
#define CHOOSE_MOVETO	(CHOOSE_NUMBER - 2)

#define LOGFILENAMEFMT	"notify-guid%i.log"
#define LOGFILENAMEBUF	32			/* need to recalculate this if LOGFILENAMEFMT is changed */

LS Notify **endoflist;
LS int lock_ison = FALSE;

void purge_notify(void)
{
	Notify	*nf;
	nfLog	*nlog;

	/*
	 *  empty the notifylist
	 */
	while(current->notifylist)
	{
		nf = current->notifylist;
		current->notifylist = nf->next;
		while(nf->log)
		{
			nlog = nf->log;
			nf->log = nlog->next;
			Free((char**)&nlog);
		}
		Free((char**)&nf);
	}
}

int mask_check(Notify *nf, char *userhost)
{
	char	*mask,*rest;
	int	ret;

	/*
	 *  no mask (NULL)
	 */
	if (!nf->mask)
		return(NF_MASKONLINE);

	ret = NF_NOMATCH;
	if (nf->endofmask)
	{
		/*
		 *  multiple masks separated by spaces
		 */
		mask = rest = nf->mask;
		chop(&rest);
	}
	else
	{
		/*
		 *  single mask
		 */
		if (!matches(nf->mask,userhost))
			ret = NF_MASKONLINE;
		return(ret);
	}
	while(mask)
	{
		if (!matches(mask,userhost))
		{
			ret = NF_MASKONLINE;
			break;
		}
		mask = chop(&rest);
	}
	if (nf->endofmask)
	{
		/*
		 *  undo the chop()'s
		 */
		for(mask=nf->mask;mask<nf->endofmask;mask++)
		{
			if (*mask == 0)
				*mask = ' ';
		}
	}
	return(ret);
}

void send_ison(void)
{
	Notify	*nf,*chosen[CHOOSE_NUMBER];
	char	isonmsg[MSGLEN];
	char	*p,*src;
	int	i,x,period;

	/*
	 *  dont send nicks to ISON too often
	 */
	period = current->setting[INT_ISONDELAY].int_var;
	x = now - current->lastnotify;
	if ((x < period) || (lock_ison && (x < 600)))
		return;

	current->lastnotify = now;

	/*
	 *  the nature of the code makes it so that the first NULL is
	 *  pushed further down the list as more entries are added,
	 *  so no need to blank the whole list
	 */
	chosen[0] = NULL;

	/*
	 *  select the oldest (CHOOSE_NUMBER) nicks for an update
	 */
	for(nf=current->notifylist;nf;nf=nf->next)
	{
		for(i=0;i<CHOOSE_NUMBER;i++)
		{
			if (!chosen[i] || (chosen[i]->checked > nf->checked))
			{
				for(x=CHOOSE_MOVETO;x>=i;x--)
					chosen[x+1] = chosen[x];
				chosen[i] = nf;
				break;
			}
		}
	}
	if (chosen[0])
	{
		p = isonmsg;
		for(i=0;i<CHOOSE_NUMBER;i++)
		{
			/*
			 *  drop out once the end-of-chosen-list NULL is found
			 */
			if (!chosen[i])
				break;
			chosen[i]->checked = 1;
			src = chosen[i]->nick;
			if (i) *(p++) = ' ';
			while((*p = *(src++))) p++;
		}
		to_server("ISON :%s\n",isonmsg);
		lock_ison = TRUE;
	}
}

void catch_ison(char *rest)
{
	Notify	*nf;
	char	whoismsg[MSGLEN];
	char	*nick,*dst;

	lock_ison = FALSE;
	*whoismsg = 0;
	dst = whoismsg;
	while((nick = chop(&rest)))
	{
		for(nf=current->notifylist;nf;nf=nf->next)
		{
			if (!nickcmp(nf->nick,nick))
			{
				nf->checked = now;
				/*
				 *  /whois user to get user@host + realname
				 */
				if (nf->status == NF_OFFLINE)
				{
					if (*whoismsg) *(dst++) = ',';
					*dst = 0;
					dst = Strcat(dst,nf->nick);
					nf->status = NF_WHOIS;
				}
			}
		}
	}

	if (*whoismsg)
		to_server("WHOIS %s\n",whoismsg);

	for(nf=current->notifylist;nf;nf=nf->next)
	{
		if (nf->checked == 1)
		{
			nf->checked = now;
			if (nf->status >= NF_WHOIS)
			{
				/*
				 *  close the log entry for this online period
				 */
				if (nf->log && nf->log->signon && !nf->log->signoff)
					nf->log->signoff = now;
				/*
				 *  announce that the user is offline if its a mask match
				 */
				if (nf->status == NF_MASKONLINE)
					send_spy(SPYSTR_STATUS,"Notify: %s is offline",nf->nick);
				nf->status = NF_OFFLINE;
			}
		}
	}
}

void catch_whois(char *nick, char *userhost, char *realname)
{
	Notify	*nf;
	nfLog	*nlog;

	if (!realname || !*realname)
		realname = "unknown";

	for(nf=current->notifylist;nf;nf=nf->next)
	{
		if ((nf->status == NF_WHOIS) && !nickcmp(nf->nick,nick))
		{
			/*
			 *  put in a new log entry
			 */
			set_mallocdoer(catch_whois);
			nlog = (nfLog*)Calloc(sizeof(nfLog) + Strlen2(userhost,realname)); // realname is never NULL
			nlog->signon = now;
			nlog->next = nf->log;
			nf->log = nlog;
			nlog->realname = Strcat(nlog->userhost,userhost) + 1;
			Strcpy(nlog->realname,realname);
			/*
			 *  if there is a mask, we need a match to announce the online status
			 */
			nf->status = mask_check(nf,userhost);
			if (nf->status == NF_MASKONLINE)
				send_spy(SPYSTR_STATUS,"Notify: %s (%s) is online",nf->nick,userhost);
			return;
		}
	}
}

/*
 *
 *  saving and loading notify information
 *
 */

int notifylog_callback(char *rest)
{
	Notify	*nf;
	nfLog	*nlog,**pp;
	time_t	on,off;
	char	*nick,*userhost;

	if (*rest == COMMENT_CHAR)
		return(FALSE);

	nick = chop(&rest);

	on = a2i(chop(&rest));
	if (errno)
		return(FALSE);

	off = a2i(chop(&rest));
	if (errno)
		return(FALSE);

	userhost = chop(&rest);

	if (rest && *rest == ':')
		rest++;
	if (!*rest)
		return(FALSE);

	for(nf=current->notifylist;nf;nf=nf->next)
	{
		if (!nickcmp(nick,nf->nick))
		{
			pp = &nf->log;
			while(*pp)
			{
				if ((*pp)->signon < on)
					break;
				pp = &(*pp)->next;
			}
			set_mallocdoer(notifylog_callback);
			nlog = (nfLog*)Calloc(sizeof(nfLog) + Strlen2(userhost,rest)); // rest is never NULL
			nlog->signon = on;
			nlog->signoff = off;
			nlog->next = *pp;
			*pp = nlog;
			nlog->realname = Strcat(nlog->userhost,userhost) + 1;
			Strcpy(nlog->realname,rest);
			break;
		}
	}
	return(FALSE);
}

void read_notifylog(void)
{
	char	fname[LOGFILENAMEBUF];
	int	fd;
#ifdef DEBUG
	int	dd;
#endif /* DEBUG */

	sprintf(fname,LOGFILENAMEFMT,current->guid);
	if ((fd = open(fname,O_RDONLY)) < 0)
		return;
#ifdef DEBUG
	dd = dodebug;
	dodebug = FALSE;
#endif /* DEBUG */

	readline(fd,&notifylog_callback);		/* readline closes fd */

#ifdef DEBUG
	dodebug = dd;
#endif /* DEBUG */
}

void write_notifylog(void)
{
	Notify	*nf;
	nfLog	*nlog;
	char	fname[LOGFILENAMEBUF];
	int	fd;
#ifdef DEBUG
	int	dd;
#endif /* DEBUG */

	sprintf(fname,LOGFILENAMEFMT,current->guid);
	if ((fd = open(fname,O_WRONLY|O_CREAT|O_TRUNC,NEWFILEMODE)) < 0)
		return;

#ifdef DEBUG
	dd = dodebug;
	dodebug = FALSE;
#endif /* DEBUG */
	for(nf=current->notifylist;nf;nf=nf->next)
	{
		to_file(fd,COMMENT_STRCHR "\n" COMMENT_STRCHR " Nick: %s\n",nf->nick);
		if (nf->info)
			to_file(fd,COMMENT_STRCHR " Note: %s\n",nf->info);
		if (nf->mask)
			to_file(fd,COMMENT_STRCHR " Mask: %s\n",nf->mask);
		to_file(fd,COMMENT_STRCHR "\n");
		for(nlog=nf->log;nlog;nlog=nlog->next)
		{
			to_file(fd,"%s %lu %lu %s :%s\n",nf->nick,nlog->signon,
				(nlog->signoff) ? nlog->signoff : now,
				nlog->userhost,nlog->realname);
		}
	}
	close(fd);
#ifdef DEBUG
	dodebug = dd;
#endif /* DEBUG */
}


int notify_callback(char *rest)
{
	Notify	*nf;
	char	*nick;
	char	*src,*dst;
	char	*lspace;

	errno = EINVAL;

	if (!rest || *rest == COMMENT_CHAR)
		return(FALSE);
	fix_config_line(rest);
	if ((nick = chop(&rest)) == NULL)
		return(FALSE);

#ifdef DEBUG
	debug("(notify_callback) parsing %s `%s'\n",nick,nullstr(rest));
#endif /* DEBUG */

	lspace = rest - 1;
	src = dst = rest;
	while(*src)
	{
		if (*src == ' ')
		{
			if (!lspace)
			{
				lspace = dst;
				*(dst++) = *src;
			}
			src++;
		}
		else
		if (*src == ':' && lspace)
		{
			*lspace = 0;
			break;
		}
		else
		{
			lspace = NULL;
			*(dst++) = *(src++);
		}
	}

	if (*src == ':')
		*(src++) = 0;
	if (!*src)
		src = NULL;

#ifdef DEBUG
	debug("(notify_callback) creating struct\n");
#endif /* DEBUG */

	/*
	 *  nick = nick
	 *  rest = mask(s) or *rest == 0
	 *  src = description or NULL
	 */
	set_mallocdoer(notify_callback);
	nf = (Notify*)Calloc(sizeof(Notify) + StrlenX(nick,rest,src,NULL));
	dst = Strcat(nf->nick,nick);
	if (*rest)
	{
		nf->mask = dst + 1;
		dst = Strcat(nf->mask,rest);
		if (STRCHR(nf->mask,' '))
			nf->endofmask = dst;
	}
	if (src)
	{
		nf->info = dst + 1;
		Strcpy(nf->info,src);
	}
	/* put it at the end of notifylist */
	*endoflist = nf;
	endoflist = &nf->next;

	errno = 0;
	return(FALSE);
}

int read_notify(char *filename)
{
	int	fd;

	if (!filename)
		return(FALSE);

	if ((fd = open(filename,O_RDONLY)) < 0)
		return(FALSE);

	/*
	 *  save online logs
	 */
	if (current->notifylist)
		write_notifylog();

	/*
	 *  delete the whole list
	 */
	purge_notify();

	endoflist = &current->notifylist;
	readline(fd,&notify_callback);			/* readline closes fd */

	/*
	 *  read back online logs
	 */
	read_notifylog();

	return(TRUE);
}

/*
 *
 *  commands...
 *
 */

#define NF_OPTIONS	7

LS const char notify_opt[NF_OPTIONS][10] =
{
"-ALL",
"-NOMATCH",
"-RELOAD",
"-FULL",
"-SEEN",
};

#define NFF_ALL		1
#define NFF_NOMATCH	2
#define NFF_RELOAD	4
#define NFF_FULL	8
#define NFF_SEEN	16

LS char *nf_from;
LS int nf_header;

void nfshow_brief(Notify *nf)
{
	time_t	when;
	char	mem[40];
	char	*s;
	int	d,h,m;

	if (nf->status == NF_NOMATCH)
		s = " nickname in use";
	else
	if (nf->status >= NF_WHOIS)
		s = " online now";
	else
	if (nf->log && nf->log->signoff)
	{
		s = mem;
		when = now - nf->log->signoff;
		d = when / 86400;
		h = (when -= d * 86400) / 3600;
		m = (when -= h * 3600) / 60;
		sprintf(mem,"%2i day%1s %02i:%02i ago",d,EXTRA_CHAR(d),h,m);
	}
	else
		s = " never seen";

	if (!nf_header)
		to_user(nf_from,"\037nick\037         \037last seen\037             \037note\037");
	to_user(nf_from,(nf->info) ? "%-9s   %-22s %s" : "%-9s   %s",
		nf->nick,s,nf->info);
	nf_header++;
}

void nfshow_full(Notify *nf)
{
	char	mem[MSGLEN];
	nfLog	*nlog;
	char	*s,*opt;

	if (nf_header)
		to_user(nf_from," ");
	to_user(nf_from,(nf->status == NF_MASKONLINE) ? "Nick: \037%s\037" : "Nick: %s",nf->nick);
	if (nf->info)
		to_user(nf_from,"Note: %s",nf->info);
	if (nf->mask)
		to_user(nf_from,"Mask: %s",nf->mask);
	if (nf->log)
	{
		to_user(nf_from,"Online history:");
		for(nlog=nf->log;nlog;nlog=nlog->next)
		{
			opt = mem;
			s = time2away(nlog->signon);
			if (s[1] == ':')
				*(opt++) = ' ';
			*opt = 0;
			opt = Strcat(opt,s);
			while(opt < (mem+18))
				*(opt++) = ' ';
			*opt = 0;
			opt = Strcat(opt," -- ");
			if (nlog->signoff)
			{
				s = time2away(nlog->signoff);
				if (s[1] == ':')
					*(opt++) = ' ';
				*opt = 0;
			}
			else
			{
				s = "   online now";
			}
			opt = Strcat(opt,s);
			while(opt < (mem+41))
				*(opt++) = ' ';
			*opt = 0;
			s = (nlog->realname) ? "%s: %s (%s)" : "%s: %s";
			to_user(nf_from,s,mem,nlog->userhost,nlog->realname);
		}
	}
	nf_header++;
}

void sub_notifynick(char *from, char *rest)
{
	Notify	*nf,**pp;
	char	*nick;
	int	i;

	nick = chop(&rest);
	if (!nick)
	{
		usage(from);
		return;
	}
	i = 0;
	pp = &current->notifylist;
	while(*pp)
	{
		nf = *pp;
		if (!nickcmp(nick,nf->nick))
		{
			*pp = nf->next;
			Free((char**)&nf);
			i++;
		}
		else
		{
			pp = &nf->next;
		}
	}
	if (!i)
		to_user(from,"Nick not found: %s",nick);
	else
		to_user(from,"Nick removed from notify: %s",nick);
}

void do_notify(COMMAND_ARGS)
{
	char	message[MSGLEN];
	Notify	*nf;
	nfLog	*nlog;
	char	*opt;
	int	n,flags;

	nf_from = from;
	flags = nf_header = 0;
	*message = 0;

	if (*rest)
	{
		while((opt = chop(&rest)))
		{
			if (!Strcmp(opt,"+"))
			{
				endoflist = &current->notifylist;
				while(*endoflist)
					endoflist = &(*endoflist)->next;
				notify_callback(rest);
				return;
			}
			if (!Strcmp(opt,"-"))
			{
				sub_notifynick(from,rest);
				return;
			}
			for(n=0;n<NF_OPTIONS;n++)
			{
				if (!Strcasecmp(notify_opt[n],opt))
				{
					flags |= (1 << n);
					break;
				}
			}
			if (n>=NF_OPTIONS)
			{
				for(nf=current->notifylist;nf;nf=nf->next)
				{
					if (!nickcmp(opt,nf->nick))
					{
						if (flags & NFF_FULL)
							nfshow_full(nf);
						else
							nfshow_brief(nf);
						break;
					}
				}
				if (!nf)
				{
					if (*message)
						Strcat(message,", ");
					Strcat(message,opt);
				}
				nf_header++;
			}
		}
	}

	if (*message)
	{
#ifdef DEBUG
		debug("(do_notify) dumping errnames\n");
#endif /* DEBUG */
		to_user(from,"User%s not found: %s",(STRCHR(message,',')) ? "s" : "",message);
	}

	if (nf_header)
		return;

	if (flags & NFF_RELOAD)
	{
		opt = current->setting[STR_NOTIFYFILE].str_var;
		if (opt && read_notify(opt))
		{
			flags = get_useraccess(from,"");
			send_spy(SPYSTR_STATUS,"Notify: %s reloaded by %s[%i]",
				opt,CurrentNick,flags);
			to_user(from,"notify list read from file %s",opt);
		}
		else
		{
			to_user(from,"notify list could not be read from file %s",nullstr(opt));
		}
		return;
	}

	for(nf=current->notifylist;nf;nf=nf->next)
	{
		if ((nf->status == NF_MASKONLINE) || (flags & NFF_ALL) || ((flags & NFF_NOMATCH) && (nf->status == NF_NOMATCH)))
			goto show_it;
		if ((flags & NFF_SEEN) && nf->log)
		{
			for(nlog=nf->log;nlog;nlog=nlog->next)
			{
				if (mask_check(nf,nlog->userhost) == NF_MASKONLINE)
					goto show_it;
			}
		}
		continue;
	show_it:
		if (flags & NFF_FULL)
			nfshow_full(nf);
		else
			nfshow_brief(nf);
	}
	if (!nf_header)
	{
		to_user(from,"no notify users are online");
	}
}

#endif /* NOTIFY */
