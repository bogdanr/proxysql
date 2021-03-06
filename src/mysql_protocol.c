#include "proxysql.h"



mysql_data_stream_t * mysql_data_stream_init(int fd, mysql_session_t *sess) {
//	mysql_data_stream_t *my=g_slice_new(mysql_data_stream_t);
	mysql_data_stream_t *my=stack_alloc(&myds_pool);
	my->bytes_info.bytes_recv=0;
	my->bytes_info.bytes_sent=0;
	my->pkts_recv=0;
	my->pkts_sent=0;
	pthread_mutex_lock(&conn_queue_pool.mutex);
	queue_init(&my->input.queue);
	queue_init(&my->output.queue);
	pthread_mutex_unlock(&conn_queue_pool.mutex);
	my->input.pkts=g_ptr_array_new();
	my->output.pkts=g_ptr_array_new();
	my->input.mypkt=NULL;
	my->output.mypkt=NULL;
	my->input.partial=0;
	my->output.partial=0;
	my->fd=fd;
	my->active=TRUE;
	my->sess=sess;
	return my;
}

void mysql_data_stream_close(mysql_data_stream_t *my) {
	pthread_mutex_lock(&conn_queue_pool.mutex);
	queue_destroy(&my->input.queue);
	queue_destroy(&my->output.queue);
	pthread_mutex_unlock(&conn_queue_pool.mutex);

	pkt *p;
/*
	if(my->input.mypkt) {
		if(my->input.mypkt->data)
		g_slice_free1(my->input.mypkt->length, my->input.mypkt->data);
#ifdef PKTALLOC
		mypkt_free(my->input.mypkt,my->sess);
#else
		g_slice_free1(sizeof(pkt), my->input.mypkt);
#endif
	}

	if(my->output.mypkt) {
		if(my->output.mypkt->data)
		g_slice_free1(my->output.mypkt->length, my->output.mypkt->data);
//		g_slice_free1(sizeof(pkt), my->output.mypkt);
	}
*/
	while (my->input.pkts->len) {
		p=g_ptr_array_remove_index(my->input.pkts, 0);
		g_slice_free1(p->length, p->data);
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
		debug_print("%s\n", "mypkt_free");
#endif
		mypkt_free(p,my->sess);
#else
		g_slice_free1(sizeof(pkt),p);
#endif
	}
	while (my->output.pkts->len) {
		p=g_ptr_array_remove_index(my->output.pkts, 0);
		g_slice_free1(p->length, p->data);
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
		debug_print("%s\n", "mypkt_free");
#endif
		mypkt_free(p,my->sess);
#else
		g_slice_free1(sizeof(pkt),p);
#endif
	}


	g_ptr_array_free(my->input.pkts,TRUE);
	g_ptr_array_free(my->output.pkts,TRUE);
//	g_slice_free1(sizeof(mysql_data_stream_t),my);
	stack_free(my,&myds_pool);
}


char *user_password(char *username) {
	char *ret=NULL; char *pass=NULL;
	pthread_rwlock_rdlock(&glovars.rwlock_usernames);
	ret=g_hash_table_lookup(glovars.usernames,username);
	if (ret) {
		pass=strdup(ret);
	}
	pthread_rwlock_unlock(&glovars.rwlock_usernames);
	return pass;
}



inline int mysql_pkt_get_size (pkt *p) {
	return p->length-sizeof(mysql_hdr);
}

inline enum MySQL_response_type mysql_response(pkt *p) {
	unsigned char c=*((char *)p->data+sizeof(mysql_hdr));
	switch (c) {
		case 0:
#ifdef DEBUG_COM
			debug_print("Packet %s\n", "OK_Packet");
#endif
			return OK_Packet;
		case 0xff:
#ifdef DEBUG_COM
			debug_print("Packet %s\n", "ERR_Packet");
#endif
			return ERR_Packet;
		case 0xfe:
			if ((p->length-sizeof(mysql_hdr)) < 9) {
#ifdef DEBUG_COM
				debug_print("Packet %s\n", "EOF_Packet");
#endif
				return EOF_Packet;
			}
		default:
#ifdef DEBUG_COM
			debug_print("Packet %s\n", "UNKNOWN_Packet");
#endif
			return UNKNOWN_Packet;
	}
}

gboolean query_is_cachable(mysql_session_t *sess, const char *query, int length) {
	if (glovars.mysql_query_cache_enabled==FALSE) {
		return FALSE;
	}
	gboolean ret=FALSE;
	gboolean r;
	char *query_copy=NULL;
	GRegex *regex;
	GMatchInfo *match_info;
	GError *error = NULL;


	//regex = g_regex_new ("^SELECT.*(^\\s+FOR\\s+UPDATE\\s*$)", G_REGEX_CASELESS, 0, NULL);
//	regex = g_regex_new ("^SELECT ", G_REGEX_CASELESS, 0, NULL);
//	g_regex_match_full (regex, string, -1, 0, 0, &match_info, &error);
	ret = g_regex_match (sess->regex[0], query, 0, &match_info);
//	while (g_match_info_matches (match_info))
//	{
//	}
	g_match_info_free (match_info);
////	g_regex_unref (regex);
	if (ret==FALSE) { return ret; }

	// we need a NULL terminated query, thus we copy it
	if ((query_copy=malloc(length+1))==NULL) { exit(EXIT_FAILURE); }
	query_copy[length]='\0';
	memcpy(query_copy,query,length);

	//regex = g_regex_new ("FOR UPDATE$", G_REGEX_CASELESS | G_REGEX_DOLLAR_ENDONLY, 0, NULL);
////	regex = g_regex_new ("\\s+FOR\\s+UPDATE\\s*$", G_REGEX_CASELESS , 0, NULL);
	r = g_regex_match (sess->regex[1], query_copy, 0, &match_info);
	//r = g_regex_match (regex, query, 0, &match_info);
	g_match_info_free (match_info);
////	g_regex_unref (regex);
	if (r==TRUE) { ret=FALSE; }
	


	return_query_is_cachable:
	if (query_copy) {
		free(query_copy);	
	}
	return ret;
}


int check_client_authentication_packet(pkt *mypkt, mysql_session_t *sess) {
	// WARNING : for now it only checks the password
	int ret=-1;
	int cur=sizeof(mysql_hdr)+32;
	unsigned char *username=mypkt->data+cur;
	sess->mysql_username=strdup(username);
	unsigned char *scramble_reply=NULL;
	cur+=strlen(username);
	cur++;
	unsigned char c=*((unsigned char *)mypkt->data+cur);
	cur++;
	if (c) {
		scramble_reply=mypkt->data+cur;
		cur+=c;
	}
#ifdef DEBUG_auth
	debug_print("Username = %s\n" , username);
#endif
	sess->mysql_password=user_password(username);
	if (sess->mysql_password==NULL)	 {
#ifdef DEBUG_auth
		debug_print("Username %s does not exist\n" , username);
#endif
		return ret;
	}
#ifdef DEBUG_auth
	debug_print("Password in hash table = %s\n" , sess->mysql_password);
#endif
	unsigned char reply[SHA_DIGEST_LENGTH+1];
	reply[SHA_DIGEST_LENGTH]='\0';
	if (scramble_reply) {
		proxy_scramble(reply, sess->scramble_buf, sess->mysql_password);
		if (memcmp(reply,scramble_reply,SHA_DIGEST_LENGTH)==0) {
			ret=0;
#ifdef DEBUG_auth
			debug_print("%s\n", "Password match!");
#endif
		}
	} else {
		if (strcmp(sess->mysql_password,"")==0) {
			ret=0;
#ifdef DEBUG_auth
			debug_print("%s\n", "Empty password match!");
#endif
		}
	}
	if (!ret) {
		sess->mysql_schema_cur=strdup(mypkt->data+cur);
		cur+=strlen(sess->mysql_schema_cur);
		if ((cur==(mypkt->length-1)) || (strlen(sess->mysql_schema_cur)==0) ) {
			free(sess->mysql_schema_cur);
			sess->mysql_schema_cur=NULL;
		}
#ifdef DEBUG_auth
		debug_print("Username = %s, schema = %s pkt_length = %d\n" , username, sess->mysql_schema_cur, mypkt->length);
#endif
	}
	return ret;
}


void create_handshake_packet(pkt *mypkt, char *scramble_buf) {
	mysql_hdr myhdr;
	myhdr.pkt_id=0;
	myhdr.pkt_length=sizeof(glovars.protocol_version)
		+ (strlen(glovars.server_version)+1)
		+ sizeof(uint32_t)  // thread_id
		+ 8  // scramble1
		+ 1  // 0x00
		+ sizeof(glovars.server_capabilities)
		+ sizeof(glovars.server_language)
		+ sizeof(glovars.server_status)
		+ 3 // unknown stuff
		+ 10 // filler
		+ 12 // scramble2
		+ 1  // 0x00
		+ (strlen("mysql_native_password")+1);

	mypkt->length=myhdr.pkt_length+sizeof(mysql_hdr);
	mypkt->data=g_slice_alloc0(mypkt->length);
	memcpy(mypkt->data, &myhdr, sizeof(mysql_hdr));
	int l;
	l=sizeof(mysql_hdr);
	//srand(pthread_self());
	//uint32_t thread_id=rand()%100000;
	//uint32_t thread_id=__sync_fetch_and_add(&glovars.thread_id,1);
	uint32_t thread_id=pthread_self();

	rand_struct_t rand_st;
	//randominit(&rand_st,rand(),rand());
	rand_st.max_value= 0x3FFFFFFFL;
	rand_st.max_value_dbl=0x3FFFFFFFL;
	rand_st.seed1=rand()%rand_st.max_value;
	rand_st.seed2=rand()%rand_st.max_value;

	memcpy(mypkt->data+l, &glovars.protocol_version, sizeof(glovars.protocol_version)); l+=sizeof(glovars.protocol_version);
	memcpy(mypkt->data+l, glovars.server_version, strlen(glovars.server_version)); l+=strlen(glovars.server_version)+1;
	memcpy(mypkt->data+l, &thread_id, sizeof(uint32_t)); l+=sizeof(uint32_t);
#ifdef MARIADB_BASE_VERSION
	proxy_create_random_string(scramble_buf+0,8,(struct my_rnd_struct *)&rand_st);
#else
	proxy_create_random_string(scramble_buf+0,8,(struct rand_struct *)&rand_st);
#endif
	
	int i;
	for (i=0;i<8;i++) {
		if (scramble_buf[i]==0) {
			scramble_buf[i]='a';
		}
	}
	
	memcpy(mypkt->data+l, scramble_buf+0, 8); l+=8;
	l+=1; //0x00
	memcpy(mypkt->data+l,&glovars.server_capabilities, sizeof(glovars.server_capabilities)); l+=sizeof(glovars.server_capabilities);
	memcpy(mypkt->data+l,&glovars.server_language, sizeof(glovars.server_language)); l+=sizeof(glovars.server_language);
	memcpy(mypkt->data+l,&glovars.server_status, sizeof(glovars.server_status)); l+=sizeof(glovars.server_status);
	memcpy(mypkt->data+l,"\x0f\x80\x15",3); l+=3;
	l+=10; //filler
	//create_random_string(mypkt->data+l,12,(struct my_rnd_struct *)&rand_st); l+=12;
#ifdef MARIADB_BASE_VERSION
	proxy_create_random_string(scramble_buf+8,12,(struct my_rnd_struct *)&rand_st);
#else
	proxy_create_random_string(scramble_buf+8,12,(struct rand_struct *)&rand_st);
#endif
//	create_random_string(scramble_buf+8,12,&rand_st);

	for (i=8;i<20;i++) {
		if (scramble_buf[i]==0) {
			scramble_buf[i]='a';
		}
	}

	memcpy(mypkt->data+l, scramble_buf+8, 12); l+=12;
	l+=1; //0x00
	memcpy(mypkt->data+l,"mysql_native_password",strlen("mysql_native_password"));
}



void create_ok_packet(pkt *mypkt, unsigned int id) {
	mysql_hdr myhdr;
	myhdr.pkt_id=id;
	myhdr.pkt_length=7;
	mypkt->length=myhdr.pkt_length+sizeof(mysql_hdr);
	mypkt->data=g_slice_alloc(mypkt->length);
	memcpy(mypkt->data, &myhdr, sizeof(mysql_hdr));
	memcpy(mypkt->data+sizeof(mysql_hdr),"\x00\x00\x00\x02\x00\x00\x00",7);
}

void create_err_packet(pkt *mypkt, unsigned int id, uint16_t errcode, char *errstr) {
	mysql_hdr myhdr;
	myhdr.pkt_id=id;
	myhdr.pkt_length=3+strlen(errstr);
	mypkt->length=myhdr.pkt_length+sizeof(mysql_hdr);
	mypkt->data=g_slice_alloc(mypkt->length);
	memcpy(mypkt->data, &myhdr, sizeof(mysql_hdr));
	memcpy(mypkt->data+sizeof(mysql_hdr),"\xff",1);
	memcpy(mypkt->data+sizeof(mysql_hdr)+1,&errcode,sizeof(uint16_t));
	memcpy(mypkt->data+sizeof(mysql_hdr)+3,errstr,strlen(errstr));
}


int authenticate_mysql_client(mysql_session_t *sess) {
	// generate a handshake
	pkt *hs;
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
	debug_print("%s\n", "mypkt_alloc");
#endif
	hs=mypkt_alloc(sess);
#else
	hs=g_slice_alloc(sizeof(pkt));
#endif
	create_handshake_packet(hs,sess->scramble_buf);

	// send it to the client
	if (write_one_pkt_to_net(sess->client_myds,hs)==FALSE) {
		//mysql_session_close(sess); return NULL;
		return -1;
	}

	hs=read_one_pkt_from_net(sess->client_myds);
	if (hs==NULL) {
		return -1;
		//mysql_session_close(sess); return NULL;
	}
//  if (hs->length > 52 ) {
		//r=check_client_authentication_packet(hs,sess->scramble_buf);
		sess->ret=check_client_authentication_packet(hs,sess);
//  } else {
//	  r=0;
//  }
	// free the packet
	g_slice_free1(hs->length, hs->data);
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
	debug_print("%s\n", "mypkt_free");
#endif
	mypkt_free(hs,sess);
#else
	g_slice_free1(sizeof(pkt), hs);
#endif
	if (sess->ret) {
		authenticate_mysql_client_send_ERR(sess, 1045, "#28000Access denied for user");
//		hs=g_slice_alloc(sizeof(pkt));
//		create_err_packet(hs, 2, 1045, "#28000Access denied for user");
//		write_one_pkt_to_net(sess->client_myds,hs);
		return -1;
		//mysql_session_close(sess); return NULL;
	}
	if (sess->mysql_schema_cur==NULL) {
		sess->mysql_schema_cur=strdup(glovars.mysql_default_schema);
	}

	return 0;
}

void authenticate_mysql_client_send_OK(mysql_session_t *sess) {
	// prepare an ok packet
	pkt *hs;
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
	debug_print("%s\n", "mypkt_alloc");
#endif
	hs=mypkt_alloc(sess);
#else
	hs=g_slice_alloc(sizeof(pkt));
#endif
	create_ok_packet(hs,2);
	// send it to the client
	write_one_pkt_to_net(sess->client_myds,hs);
}

void authenticate_mysql_client_send_ERR(mysql_session_t *sess, uint16_t errcode, char *errstr) {
	pkt *hs;
#ifdef PKTALLOC
#ifdef DEBUG_pktalloc
	debug_print("%s\n", "mypkt_alloc");
#endif
	hs=mypkt_alloc(sess);
#else
	hs=g_slice_alloc(sizeof(pkt));
#endif
//	create_err_packet(hs, 2, 1045, "#28000Access denied for user");
	create_err_packet(hs, 2, errcode, errstr);
	write_one_pkt_to_net(sess->client_myds,hs);
}

int mysql_check_alive_and_read_only(const char *hostname, uint16_t port) {
	MYSQL *conn=mysql_init(NULL);
	if (conn==NULL) {
		exit(EXIT_FAILURE);
	}
	if (mysql_real_connect(conn, hostname, glovars.mysql_usage_user, glovars.mysql_usage_password, NULL, port, NULL, 0) == NULL) {
		fprintf(stderr, "FATAL: server %s\n", mysql_error(conn));
		mysql_close(conn);
		return -1;
	}
	if (mysql_query(conn,"SHOW GLOBAL VARIABLES LIKE 'read_only'")) {
		mysql_close(conn);
		return -1;
	}
	MYSQL_RES *result = mysql_store_result(conn);
	if (result == NULL) {
		mysql_close(conn);
		return -1;
	}
	int num_rows = mysql_num_rows(result);
	if (num_rows != 1) {
		mysql_close(conn);
		return -1;
	}
	int num_fields = mysql_num_fields(result);
	if (num_fields != 2) {
		mysql_close(conn);
		return -1;
	}
	MYSQL_ROW row;
	row = mysql_fetch_row(result);
	if (row == NULL) {
		mysql_close(conn);
		return -1;
	}
	char *read_only;
	read_only=strdup(row[1]);
	fprintf(stderr, "server %s read_only %s\n" , hostname, read_only);
	int r=1;
	if ((strcmp(read_only,"0") == 0) || (strcmp(read_only,"OFF") == 0)) {
		r=0;
	}
	free(read_only);
	mysql_free_result(result);
	mysql_close(conn);
	return r;
}



void proxy_create_random_string(char *to, uint length, struct rand_struct *rand_st) {
	int i;
	for (i=0; i<length ; i++) {
		*to= (char) (proxy_my_rnd(rand_st) * 94 + 33);
		to++;
	}
	*to= '\0';
}

inline double proxy_my_rnd(struct rand_struct *rand_st) {
	rand_st->seed1= (rand_st->seed1*3+rand_st->seed2) % rand_st->max_value;
	rand_st->seed2= (rand_st->seed1+rand_st->seed2+33) % rand_st->max_value;
	return (((double) rand_st->seed1) / rand_st->max_value_dbl);
}



void proxy_scramble(char *to, const char *message, const char *password)
{
  uint8 hash_stage1[SHA_DIGEST_LENGTH];
  uint8 hash_stage2[SHA_DIGEST_LENGTH];

  /* Two stage SHA1 hash of the password. */
  proxy_compute_two_stage_sha1_hash(password, strlen(password), hash_stage1,
                              hash_stage2);

  /* create crypt string as sha1(message, hash_stage2) */;
  proxy_compute_sha1_hash_multi((uint8 *) to, message, SCRAMBLE_LENGTH,
                          (const char *) hash_stage2, SHA_DIGEST_LENGTH);
  proxy_my_crypt(to, (const uchar *) to, hash_stage1, SCRAMBLE_LENGTH);
}

void proxy_compute_sha1_hash_multi(uint8 *digest, const char *buf1, int len1, const char *buf2, int len2) {
	SHA_CTX sha1_context;
	SHA1_Init(&sha1_context);
	SHA1_Update(&sha1_context, buf1, len1);
	SHA1_Update(&sha1_context, buf2, len2);
	SHA1_Final(digest, &sha1_context);
}


inline void proxy_compute_two_stage_sha1_hash(const char *password, size_t pass_len, uint8 *hash_stage1, uint8 *hash_stage2) {
  proxy_compute_sha1_hash(hash_stage1, password, pass_len);
  proxy_compute_sha1_hash(hash_stage2, (const char *) hash_stage1, SHA_DIGEST_LENGTH);
}

void proxy_compute_sha1_hash(uint8 *digest, const char *buf, int len) {
	SHA_CTX sha1_context;
	SHA1_Init(&sha1_context);
	SHA1_Update(&sha1_context, buf, len);
	SHA1_Final(digest, &sha1_context);
}


void proxy_my_crypt(char *to, const uchar *s1, const uchar *s2, uint len) {
  const uint8 *s1_end= s1 + len;
  while (s1 < s1_end)
    *to++= *s1++ ^ *s2++;
}
