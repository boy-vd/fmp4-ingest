/*******************************************************************************
Supplementary software media ingest specification:
https://github.com/unifiedstreaming/fmp4-ingest

Copyright (C) 2009-2019 CodeShop B.V.
http://www.code-shop.com

CMAF ingest from stored CMAF files, emulates a live encoder posting CMAF content

******************************************************************************/

#include "fmp4stream.h"
#include "curl/curl.h"
#include <iostream>
#include <fstream>
#include <exception>
#include <memory>
#include <chrono>
#include <thread>
#include <cstring>
#include <bitset>
#include <iomanip>
#include <base64.h>

using namespace fmp4_stream;
using namespace std;
bool stop_all = false;

// an ugly global cross thread variable for the cmaf presentation duration
double cmaf_presentation_duration;

size_t write_function(void *ptr, size_t size, size_t nmemb, std::string* data)
{
	data->append((char*)ptr, size * nmemb);
	return size * nmemb;
}

int get_remote_sync_epoch(uint64_t *res_time, string &wc_uri_)
{
	CURL *curl;
	CURLcode res;
	std::string response_string;

	curl = curl_easy_init();
	if (curl)
	{
		curl_easy_setopt(curl, CURLOPT_URL, wc_uri_.c_str());

		/* example.com is redirected, so we tell libcurl to follow redirection */
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_function);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

		/* Perform the request, res will get the return code */
		res = curl_easy_perform(curl);

		/* Check for errors */
		if (res != CURLE_OK)
			fprintf(stderr, "network time lookup failed: %s\n",
				curl_easy_strerror(res));

		unsigned long ul = std::stoul(response_string, nullptr, 0);
		cout << ul << endl;
		*res_time = ul ? ul : 0;

		/* always cleanup */
		curl_easy_cleanup(curl);
	}
	return 0;
}

struct push_options_t
{
	push_options_t()
		: url_("http://localhost/live/video.isml/video.ism")
		, realtime_(false)
		, daemon_(false)
		, loop_(true)
		, wc_off_(true)
		, wc_uri_("http://time.akamai.com")
		, ism_offset_(0)
		, wc_time_start_(0)
		, dont_close_(true)
		, chunked_(false)
		, drop_every_(0)
		, prime_(false)
		, dry_run_(false)
		, verbose_(2)
		, cmaf_presentation_duration_(0)
		, avail_(0)
		, avail_dur_(0)
		, announce_(60.0)
	{
	}

	static void print_options()
	{
		printf("Usage: fmp4ingest [options] <input_files>\n");
		printf(
			" [-u url]                       Publishing Point URL\n"
			" [-r, --realtime]               Enable realtime mode\n"
			" [--wc_offset]                  (boolean )Add a wallclock time offset for converting VoD (0) asset to Live \n"
			" [--ism_offset]                insert a fixed value for hte wallclock time offset instead of using a uri"
			" [--wc_uri]                     uri for fetching wall clock time default time.akamai.com \n"
			" [--close_pp]                   Close the publishing point at the end of stream or termination \n"
			" [--chunked]                    Use chunked Transfer-Encoding for POST (long running post) otherwise short running per fragment post \n"
			" [--avail]                      signal an advertisment slot every arg1 ms with duration of arg2 ms \n"
			" [--announce]                   specify the number of seconds in advance to presenation time to send an avail"
			" [--auth]                       Basic Auth Password \n"
			" [--aname]                      Basic Auth User Name \n"
			" [--sslcert]                    TLS 1.2 client certificate \n"
			" [--sslkey]                     TLS private Key \n"
			" [--sslkeypass]                 passphrase \n"
			" <input_files>                  CMAF files to ingest (.cmf[atvm])\n"

			"\n");
	}

	void parse_options(int argc, char * argv[])
	{
		if (argc > 2)
		{
			for (int i = 1; i < argc; i++)
			{
				string t(argv[i]);
				if (t.compare("-u") == 0) { url_ = string(argv[++i]); continue; }
				if (t.compare("-l") == 0 || t.compare("--loop") == 0) { loop_ = true; continue; }
				if (t.compare("-r") == 0 || t.compare("--realtime") == 0) { realtime_ = true; continue; }
				if (t.compare("--close_pp") == 0) { dont_close_ = false; continue; }
				if (t.compare("--daemon") == 0) { daemon_ = true; continue; }
				if (t.compare("--chunked") == 0) { chunked_ = true; continue; }
				if (t.compare("--wc_offset") == 0) { wc_off_ = true; continue; }
				if (t.compare("--ism_offset") == 0) { ism_offset_ = atol(argv[++i]); continue; }
				if (t.compare("--wc_uri") == 0) { wc_uri_ = string(argv[++i]); continue; }
				if (t.compare("--auth") == 0) { basic_auth_ = string(argv[++i]); continue; }
				if (t.compare("--avail") == 0) { avail_ = atoi(argv[++i]); avail_dur_= atoi(argv[++i]); continue; }
				if (t.compare("--announce") == 0) { announce_ = atof(argv[++i]); continue; }
				if (t.compare("--aname") == 0) { basic_auth_name_ = string(argv[++i]); continue; }
				if (t.compare("--sslcert") == 0) { ssl_cert_ = string(argv[++i]); continue; }
				if (t.compare("--sslkey") == 0) { ssl_key_ = string(argv[++i]); continue; }
				if (t.compare("--keypass") == 0) { ssl_key_pass_ = string(argv[++i]); continue; }
				if (t.compare("--keypass") == 0) { basic_auth_ = string(argv[++i]); continue; }
				input_files_.push_back(argv[i]);
			}

			// get the wallclock offset 
			if (wc_off_)
				get_remote_sync_epoch(&wc_time_start_, wc_uri_);
			if (ism_offset_ > 0) 
			{
				wc_time_start_ = ism_offset_;
				wc_off_ = true;
			}
		}
		else
			print_options();
	}

	string url_;
	bool realtime_;
	bool daemon_;
	bool loop_;
	bool wc_off_;

	uint64_t wc_time_start_;
	bool dont_close_;
	bool chunked_;

	unsigned int drop_every_;
	bool prime_;
	bool dry_run_;
	int verbose_;

	string ssl_cert_;
	string ssl_key_;
	string ssl_key_pass_;
	string wc_uri_; // uri for fetching wallclock time in seconds default is time.akamai.com

	string basic_auth_name_; // user name with basic auth
	string basic_auth_; // password with basic auth

	double announce_; // number of seconds to send an announce in advance
	double cmaf_presentation_duration_;
	vector<string> input_files_;

	uint64_t avail_; // insert an avail every X milli seconds
	uint64_t avail_dur_; // of duratoin Y milli seconds
	uint64_t ism_offset_;
};

struct ingest_post_state_t
{
	bool init_done_; // flag if init fragment was sent
	bool error_state_; // flag signalling error state
	uint64_t fnumber_; // fragment nnmber in queue being set
	uint64_t frag_duration_; //  fragment dration
	uint32_t timescale_; // timescale of the media track
	uint32_t offset_in_fragment_; // for partial chunked sending keep track of fragment offset
	uint64_t start_time_stamp_; // ts offset
	ingest_stream *str_ptr_; // pointer to the ingest stream
	bool is_done_; // flag set when the stream is done
	string file_name_;
	chrono::time_point<chrono::system_clock> *start_time_; // time point the post was started
	push_options_t *opt_;
};

static size_t read_callback(void *dest, size_t size, size_t nmemb, void *userp)
{
	ingest_post_state_t *st = (ingest_post_state_t *)userp;
	size_t buffer_size = size * nmemb;
	size_t nr_bytes_written = 0;

	if (st->is_done_ || stop_all) 
		return 0;

	// resending the init segment
	if (!(st->init_done_) || st->error_state_)
	{
		vector<uint8_t> init_data;
		st->str_ptr_->get_init_segment_data(init_data);

		if (st->error_state_) {
			st->offset_in_fragment_ = 0;
			st->init_done_ = false;
			st->error_state_ = false;
		}

		if ((init_data.size() - st->offset_in_fragment_) <= buffer_size)
		{
			memcpy(dest, init_data.data() + st->offset_in_fragment_, init_data.size() - st->offset_in_fragment_);
			nr_bytes_written = init_data.size() - st->offset_in_fragment_;
			st->init_done_ = true;
			st->offset_in_fragment_ = 0;
			return nr_bytes_written;
		}
		else
		{
			memcpy(dest, init_data.data(), buffer_size);
			st->offset_in_fragment_ += (uint32_t)buffer_size;
			return buffer_size;
		}
	}
	else // read the next fragment or send pause
	{
		bool frags_finished = st->str_ptr_->media_fragment_.size() <= st->fnumber_;
		uint64_t c_tfdt = 0;

		if ((frags_finished ))
		{
			uint64_t patch_shift = (uint64_t) (st->opt_->cmaf_presentation_duration_ * st->str_ptr_->init_fragment_.get_time_scale());
			if (patch_shift > 0) 
			{
				cout << "patching using cmaf presentation duration" << endl;
				st->str_ptr_->patch_tfdt(patch_shift, false);
			}
			else 
			{
				cout << "patching using cmaf presentation duration" << endl;
				st->str_ptr_->patch_tfdt(st->str_ptr_->get_duration(), false);
			}

			*(st->start_time_) = chrono::system_clock::now();
			st->fnumber_ = 0;
		}

		if (st->opt_->realtime_) // if it is to early sleep until tfdt - frag_dur
		{
			c_tfdt = st->str_ptr_->media_fragment_[st->fnumber_].tfdt_.base_media_decode_time_;
			const double media_time = ((double)c_tfdt - st->str_ptr_->get_start_time()) / st->timescale_;
			chrono::duration<double> diff = chrono::system_clock::now() - *(st->start_time_);
			double f_del = (double)st->frag_duration_ / (double)st->timescale_;

			if ((diff.count() ) < (media_time - f_del)) // if it is to early sleep until tfdt - frag_dur
			{
				this_thread::sleep_for(chrono::duration<double>((media_time - f_del) - diff.count()));
			}
		}

		vector<uint8_t> frag_data;
		st->str_ptr_->get_media_segment_data((long)st->fnumber_, frag_data);

		// we can finish the fragment
		if (frag_data.size() - st->offset_in_fragment_ <= buffer_size)
		{
			memcpy(dest, frag_data.data() + st->offset_in_fragment_, frag_data.size() - st->offset_in_fragment_);
			nr_bytes_written = frag_data.size() - st->offset_in_fragment_;
			st->offset_in_fragment_ = 0;

			std::cout << " pushed media fragment: " << st->fnumber_ << " file_name: " << st->file_name_ << " fragment duration: " << \
				((double)st->str_ptr_->media_fragment_[st->fnumber_].get_duration()) / ((double)st->timescale_) << " seconds " << endl;

			st->fnumber_++;

			return nr_bytes_written;
		}
		else 
		{
			memcpy(dest, frag_data.data() + st->offset_in_fragment_, buffer_size);
			(st->offset_in_fragment_) += (uint32_t)buffer_size;
			
			return buffer_size;
		}
	}
}

int push_thread(ingest_stream l_ingest_stream, push_options_t opt, string post_url_string, std::string file_name)
{
	try
	{		

			// check the connection with the post server
			// create databuffer with init segment
			vector<uint8_t> init_seg_dat;
			l_ingest_stream.get_init_segment_data(init_seg_dat);

			// setup curl
			CURL * curl;
			CURLcode res;
			curl = curl_easy_init();

			curl_easy_setopt(curl, CURLOPT_URL, post_url_string.c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());

			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

			if (opt.basic_auth_.size())
			{
				curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
				curl_easy_setopt(curl, CURLOPT_USERNAME, opt.basic_auth_name_.c_str());
				curl_easy_setopt(curl, CURLOPT_USERPWD, opt.basic_auth_.c_str());
			}

			if (opt.ssl_cert_.size())
				curl_easy_setopt(curl, CURLOPT_SSLCERT, opt.ssl_cert_.c_str());
			if (opt.ssl_key_.size())
				curl_easy_setopt(curl, CURLOPT_SSLKEY, opt.ssl_key_.c_str());
			if (opt.ssl_key_pass_.size())
				curl_easy_setopt(curl, CURLOPT_KEYPASSWD, opt.ssl_key_pass_.c_str());

			res = curl_easy_perform(curl);

			/* Check for errors */
			if (res != CURLE_OK)
			{
				fprintf(stderr, "---- connection with server failed  %s\n",
					curl_easy_strerror(res));
				curl_easy_cleanup(curl);
				return 0; // nothing to do
			}
			else
			{
				fprintf(stderr, "---- connection with server sucessfull %s\n",
					curl_easy_strerror(res));
			}

			// setup the post
			ingest_post_state_t post_state = {};
			post_state.error_state_ = false;
			post_state.fnumber_ = 0;
			post_state.frag_duration_ = l_ingest_stream.media_fragment_[0].get_duration();
			post_state.init_done_ = true; // the init fragment was already sent
			post_state.start_time_stamp_ = l_ingest_stream.media_fragment_[0].tfdt_.base_media_decode_time_;
			post_state.str_ptr_ = &l_ingest_stream;
			post_state.timescale_ = l_ingest_stream.init_fragment_.get_time_scale();
			post_state.is_done_ = false;
			post_state.offset_in_fragment_ = 0;
			post_state.opt_ = &opt;
			post_state.file_name_ = file_name;

			chrono::time_point<chrono::system_clock> tp = chrono::system_clock::now();
			post_state.start_time_ = &tp; // start time

			if (opt.chunked_)
				curl_easy_reset(curl);

			curl_easy_setopt(curl, CURLOPT_URL, post_url_string.data());
			curl_easy_setopt(curl, CURLOPT_POST, 1);
			curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
			
			if (opt.chunked_)
			{
				curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
				curl_easy_setopt(curl, CURLOPT_READDATA, (void *)&post_state);
			}

			struct curl_slist *chunk = NULL;
			if (opt.chunked_)
			{
				chunk = curl_slist_append(chunk, "Transfer-Encoding: chunked");
				chunk = curl_slist_append(chunk, "Expect:");
				res = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
				curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

				if (opt.basic_auth_.size())
				{
					curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
					curl_easy_setopt(curl, CURLOPT_USERNAME, opt.basic_auth_name_.c_str());
					curl_easy_setopt(curl, CURLOPT_USERPWD, opt.basic_auth_.c_str());
				}

				if (opt.ssl_cert_.size())
					curl_easy_setopt(curl, CURLOPT_SSLCERT, opt.ssl_cert_.c_str());
				if (opt.ssl_key_.size())
					curl_easy_setopt(curl, CURLOPT_SSLKEY, opt.ssl_key_.c_str());
				if (opt.ssl_key_pass_.size())
					curl_easy_setopt(curl, CURLOPT_KEYPASSWD, opt.ssl_key_pass_.c_str());
			}
			// long running post with chunked transfer
			if (opt.chunked_)
			{
				while (!stop_all) 
				{
					res = curl_easy_perform(curl);

					if (res != CURLE_OK)
					{
						fprintf(stderr, "---- long running post failed: %s\n",
								curl_easy_strerror(res));
						// wait two seconds before trying again
						post_state.offset_in_fragment_ = 0; // start again from beginning of fragment
						double f_del = (double)post_state.frag_duration_ / (double)post_state.timescale_;
						this_thread::sleep_for(chrono::duration<double>(f_del));
						post_state.error_state_ = true;
					}
					else
						//fprintf(stderr, "---- long running post ok: %s\n",
						curl_easy_strerror(res);
				}
			}
			else  // short running post 
			{
				cout << "************* short running post mode ************" << endl;
				chrono::time_point<chrono::system_clock> start_time = chrono::system_clock::now();
				while (!stop_all) 
				{

					for (uint64_t i = 0; i < l_ingest_stream.media_fragment_.size(); i++)
					{
						vector<uint8_t> media_seg_dat;
						uint64_t media_seg_size = l_ingest_stream.get_media_segment_data(i, media_seg_dat);

						curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *) &media_seg_dat[0]);
						curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)media_seg_dat.size());
						res = curl_easy_perform(curl);

						if (res != CURLE_OK)
						{
							fprintf(stderr, "post of media segment failed: %s\n",
								curl_easy_strerror(res));

							// media segment post failed resend the init segment and the segment
							int retry_count = 0;
							while (res != CURLE_OK)
							{
								// resend init segment
								curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
								curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());
								res = curl_easy_perform(curl);

								// wait for 1000 milliseconds before retrying
								std::this_thread::sleep_for(std::chrono::milliseconds(1000));
								retry_count++;
								if (retry_count == 10)
									return 0;
							}
						}
						else
						{
							fprintf(stderr, "post of media segment ok: %s\n",
								curl_easy_strerror(res));
						}

						uint64_t t_diff = l_ingest_stream.media_fragment_[i].tfdt_.base_media_decode_time_ - l_ingest_stream.media_fragment_[0].tfdt_.base_media_decode_time_;

						cout << " pushed media fragment: " << i << " file_name: " << post_state.file_name_ << " fragment duration: " << \
							(l_ingest_stream.media_fragment_[i].get_duration()) / ((double)post_state.timescale_) << " seconds ";
						if(post_state.timescale_ > 0)
						    cout << " media time elapsed: " << (t_diff + l_ingest_stream.media_fragment_[i].get_duration()) / post_state.timescale_ << endl;
						
						if (opt.realtime_)
						{
							// calculate elapsed media time
							const uint64_t c_tfdt = l_ingest_stream.media_fragment_[i].tfdt_.base_media_decode_time_;
							chrono::duration<double> diff = chrono::system_clock::now() - start_time;
							const double media_time = ((double) (c_tfdt - l_ingest_stream.get_start_time())) / l_ingest_stream.init_fragment_.get_time_scale();
		

							// wait untill media time - frag_delay > elapsed time + initial offset
							if ((diff.count()) < (media_time)) // if it is to early sleep until tfdt - frag_dur
							{
								double fdel =  (double) (l_ingest_stream.media_fragment_[i].get_duration()) / ((double)post_state.timescale_);
								// sleep but the maximum sleep time is one fragment duration
								if(fdel < (media_time - diff.count()))
								    this_thread::sleep_for(chrono::duration<double>(fdel));
								else
								    this_thread::sleep_for(chrono::duration<double>((media_time) - diff.count()));
							}

						}
						else
						{ // non real time just sleep for 500 seconds
							std::this_thread::sleep_for(std::chrono::milliseconds(500));
						}

						//std::cout << " --- posting next segment ---- " << i << std::endl;
						if (post_state.is_done_ || stop_all ) {
							i = (uint64_t)l_ingest_stream.media_fragment_.size();
							break;
						}
					}

					l_ingest_stream.patch_tfdt(opt.cmaf_presentation_duration_);
					start_time = chrono::system_clock::now();
				}
			}

			// only close with mfra if dont close is not set
			if (!opt.dont_close_)
			{
				// post the empty mfra segment
				curl_easy_setopt(curl, CURLOPT_URL, post_url_string.data());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)empty_mfra);
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)8u);
				/* Perform the request, res will get the return code */
				res = curl_easy_perform(curl);

				/* Check for errors */
				if (res != CURLE_OK)
					fprintf(stderr, "post of mfra signalling segment failed: %s\n",
						curl_easy_strerror(res));
				else
					fprintf(stderr, "post of mfra segment failed: %s\n",
						curl_easy_strerror(res));
			}
			/* always cleanup */
			curl_easy_cleanup(curl);
		
	}
	catch (...)
	{
		cout << "Unknown Error" << endl;
	}

	return 0;
}

int push_thread_meta(ingest_stream l_ingest_stream, push_options_t opt, std::string post_url_string, std::string file_name)
{
	try
	{
		vector<uint8_t> init_seg_dat;
		l_ingest_stream.get_init_segment_data(init_seg_dat);
		//l_ingest_stream.init_fragment_.

		// setup curl
		CURL * curl;
		CURLcode res;
		curl = curl_easy_init();

		curl_easy_setopt(curl, CURLOPT_URL, post_url_string.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());

		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

		if (opt.basic_auth_.size())
		{
			curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
			curl_easy_setopt(curl, CURLOPT_USERNAME, opt.basic_auth_name_.c_str());
			curl_easy_setopt(curl, CURLOPT_USERPWD, opt.basic_auth_.c_str());
		}

		if (opt.ssl_cert_.size())
			curl_easy_setopt(curl, CURLOPT_SSLCERT, opt.ssl_cert_.c_str());
		if (opt.ssl_key_.size())
			curl_easy_setopt(curl, CURLOPT_SSLKEY, opt.ssl_key_.c_str());
		if (opt.ssl_key_pass_.size())
			curl_easy_setopt(curl, CURLOPT_KEYPASSWD, opt.ssl_key_pass_.c_str());

		res = curl_easy_perform(curl);

		/* Check for errors */
		if (res != CURLE_OK)
		{
			fprintf(stderr, "---- connection with server failed  %s\n",
				curl_easy_strerror(res));
			curl_easy_cleanup(curl);
			return (0); // Exit with error when connection fails
		}
		else
		{
			fprintf(stderr, "---- connection with server sucessfull %s\n",
				curl_easy_strerror(res));
		}

		// setup the post
		chrono::time_point<chrono::system_clock> start_time = chrono::system_clock::now();

		while (!stop_all)
		{
			for (uint64_t i = 0; i < l_ingest_stream.media_fragment_.size(); i++)
			{
				if (opt.realtime_)
				{
					// calculate elapsed media time
					const uint64_t c_tfdt = l_ingest_stream.media_fragment_[i].tfdt_.base_media_decode_time_;
					chrono::duration<double> diff = chrono::system_clock::now() - start_time;
					const double media_time = ((double)(c_tfdt - l_ingest_stream.get_start_time())) / l_ingest_stream.init_fragment_.get_time_scale();

					double wait_time = media_time - opt.announce_ - diff.count();

					if (wait_time > 0) // if it is to early sleep until tfdt - frag_dur
					{
						this_thread::sleep_for(chrono::duration<double>(wait_time));
					}

				}
				else
				{ // non real time just sleep for 500 seconds
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}

				vector<uint8_t> media_seg_dat;
				uint64_t media_seg_size = l_ingest_stream.get_media_segment_data(i, media_seg_dat);

				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&media_seg_dat[0]);
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)media_seg_dat.size());
				res = curl_easy_perform(curl);

				if (res != CURLE_OK)
				{
					fprintf(stderr, "post of media segment failed: %s\n",
						curl_easy_strerror(res));

					// media segment post failed resend the init segment and the segment
					int retry_count = 0;
					while (res != CURLE_OK)
					{
						curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
						curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());
						res = curl_easy_perform(curl);
						std::this_thread::sleep_for(std::chrono::milliseconds(1000));
						retry_count++;
						if (retry_count == 10)
							return 0;
					}
				}
				else
				{
					fprintf(stderr, "post of media segment ok: %s\n",
						curl_easy_strerror(res));
				}

				//std::cout << " --- posting next segment ---- " << i << std::endl;
				if (stop_all) {
					i = (uint64_t)l_ingest_stream.media_fragment_.size();
					break;
				}
			}
			
			l_ingest_stream.patch_tfdt(opt.cmaf_presentation_duration_);

			
			start_time = chrono::system_clock::now();
		}


		// only close with mfra if dont close is not set
		if (!opt.dont_close_)
		{
			// post the empty mfra segment
			curl_easy_setopt(curl, CURLOPT_URL, post_url_string.data());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)empty_mfra);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)8u);
			/* Perform the request, res will get the return code */
			res = curl_easy_perform(curl);

			/* Check for errors */
			if (res != CURLE_OK)
				fprintf(stderr, "post of mfra signalling segment failed: %s\n",
					curl_easy_strerror(res));
			else
				fprintf(stderr, "post of mfra segment failed: %s\n",
					curl_easy_strerror(res));
		}
		/* always cleanup */
		curl_easy_cleanup(curl);

	}
	catch (...)
	{
		cout << "Unknown Error" << endl;
	}
	return 0;
}

int push_thread_emsg(push_options_t opt, std::string post_url_string, std::string file_name, uint32_t track_id, uint32_t timescale)
{
	try
	{
		vector<uint8_t> init_seg_dat;
		vector<uint8_t> moov_sparse;
		string urn = "urn:mpeg:dash:event:2012";
		sparse_ftyp;
		get_sparse_moov(urn, timescale, track_id, moov_sparse);
		std::copy(std::begin(sparse_ftyp), std::end(sparse_ftyp), std::back_inserter(init_seg_dat));
		std::copy(std::begin(moov_sparse), std::end(moov_sparse), std::back_inserter(init_seg_dat));
		//write the output
		ofstream outf = std::ofstream("emsg2.cmfm", std::ios::binary);

		// setup curl
		CURL * curl;
		CURLcode res;
		curl = curl_easy_init();

		curl_easy_setopt(curl, CURLOPT_URL, post_url_string.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());
		
		if (outf.good())
			outf.write((char *)&init_seg_dat[0], init_seg_dat.size());

		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

		if (opt.basic_auth_.size())
		{
			curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
			curl_easy_setopt(curl, CURLOPT_USERNAME, opt.basic_auth_name_.c_str());
			curl_easy_setopt(curl, CURLOPT_USERPWD, opt.basic_auth_.c_str());
		}

		if (opt.ssl_cert_.size())
			curl_easy_setopt(curl, CURLOPT_SSLCERT, opt.ssl_cert_.c_str());
		if (opt.ssl_key_.size())
			curl_easy_setopt(curl, CURLOPT_SSLKEY, opt.ssl_key_.c_str());
		if (opt.ssl_key_pass_.size())
			curl_easy_setopt(curl, CURLOPT_KEYPASSWD, opt.ssl_key_pass_.c_str());

		res = curl_easy_perform(curl);

		/* Check for errors */
		if (res != CURLE_OK)
		{
			fprintf(stderr, "---- connection with server failed  %s\n",
				curl_easy_strerror(res));
			curl_easy_cleanup(curl);
			exit(1); // Exit status 1
		}
		else
		{
			fprintf(stderr, "---- connection with server sucessfull %s\n",
				curl_easy_strerror(res));
		}

		chrono::time_point<chrono::system_clock> start_time = chrono::system_clock::now();

		double cmaf_loop_offset = 0;

		while (!stop_all)
		{
			
			for (uint64_t i = 1, j =1;; i++,j++)
			{
				double media_time = ((double)j * opt.avail_) / timescale;

				if (opt.realtime_)
				{    
					if(opt.cmaf_presentation_duration_)
					   if (media_time > opt.cmaf_presentation_duration_) 
					   {
						   j = 1;
						   cmaf_loop_offset += opt.cmaf_presentation_duration_;
						   media_time = ((double)j * opt.avail_) / timescale;
					   }

					// calculate elapsed media time
					chrono::duration<double> diff = chrono::system_clock::now() - start_time;
					double wait_time = media_time + cmaf_loop_offset - 5.0 - opt.announce_- diff.count();

					if (wait_time > 0) // if it is to early sleep until tfdt - frag_dur
					{
						this_thread::sleep_for(chrono::duration<double>(wait_time));
					}

				}
				else
				{ 
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}


				
				// create emsg
				emsg e = {};
				e.timescale_ = timescale; // 1000
				e.presentation_time_ = uint64_t  ((media_time + cmaf_loop_offset + opt.wc_time_start_) * timescale);
				e.id_ = (uint32_t) i;
				e.version_ = 0;
				e.presentation_time_delta_ = 0;
				e.event_duration_ = (uint32_t) opt.avail_dur_;
				fmp4_stream::gen_splice_insert(e.message_data_, e.id_, e.event_duration_ * 90);
				e.scheme_id_uri_ = "urn:scte:scte35:2013:bin";
				e.value_ = "";
				
				vector<uint8_t> sparse_seg_dat;
				e.convert_emsg_to_sparse_fragment(sparse_seg_dat, \
					(uint64_t) ((media_time + cmaf_loop_offset + opt.wc_time_start_) * timescale), track_id, timescale, 0);
				
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&sparse_seg_dat[0]);
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)sparse_seg_dat.size());
				res = curl_easy_perform(curl);

				if(outf.good())
				    outf.write((const char *)&sparse_seg_dat[0], sparse_seg_dat.size());

				if (res != CURLE_OK)
				{
					fprintf(stderr, "post of media segment failed: %s\n",
						curl_easy_strerror(res));

					// media segment post failed resend the init segment and the segment
					int retry_count = 0;
					while (res != CURLE_OK)
					{
						curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char *)&init_seg_dat[0]);
						curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)init_seg_dat.size());
						res = curl_easy_perform(curl);
						std::this_thread::sleep_for(std::chrono::milliseconds(1000));
						retry_count++;
						if (retry_count == 10)
							return 0;
					}
				}
				else
				{
					fprintf(stderr, "post of emsg segment with scte-35 ok: %s\n",
						curl_easy_strerror(res));
					//std::cout << "payload: " << base64_encode((unsigned char *) &e.message_data_[0], e.message_data_.size()) << std::endl;
					
				}

				if (stop_all) {
					break;
				}
			}
		}

		/* always cleanup */
		curl_easy_cleanup(curl);
		outf.close();
	}
	catch (...)
	{
		cout << "Unknown Error" << endl;
	}
	return 0;
}

int main(int argc, char * argv[])
{
	push_options_t opts;
	opts.parse_options(argc, argv);
	vector<ingest_stream> l_istreams(opts.input_files_.size());
	typedef shared_ptr<thread> thread_ptr;
	typedef vector<thread_ptr> threads_t;
	threads_t threads;
	int l_index = 0;

	for (auto it = opts.input_files_.begin(); it != opts.input_files_.end(); ++it)
	{
		ifstream input(*it, ifstream::binary);

		if (!input.good())
		{
			std::cout << "failed loading input file: [cmf[tavm]]" << string(*it) << endl;
			push_options_t::print_options();
			return 0;
		}

		ingest_stream &l_ingest_stream = l_istreams[l_index];
		l_ingest_stream.load_from_file(input);

		// patch the tfdt values with an offset time
		if (opts.wc_off_)
		{
			l_ingest_stream.patch_tfdt(opts.wc_time_start_);
		}

		double l_duration = (double) l_ingest_stream.get_duration() / l_ingest_stream.init_fragment_.get_time_scale();

		if (l_duration > opts.cmaf_presentation_duration_) {
			opts.cmaf_presentation_duration_ = l_duration;
			std::cout << "cmaf presentation duration updated to: " << l_duration << " seconds " << std::endl;
		}

		l_index++;
		input.close();
	}
	l_index = 0;

	if (opts.avail_)
	{
		string post_url_string = opts.url_ + "/Streams(" + "emsg.cmfm" + ")";
		string file_name = "emsg.cmfm";
		thread_ptr thread_n(new thread(push_thread_emsg, opts, post_url_string, file_name, 99, 1000));
		threads.push_back(thread_n);
	}

	for (auto it = opts.input_files_.begin(); it != opts.input_files_.end(); ++it)
	{
		string post_url_string = opts.url_ + "/Streams(" + *it + ")";

		if(it->substr(it->find_last_of(".") + 1) == "cmfm")
        {
			cout << "push thread: " << post_url_string << endl;
		    thread_ptr thread_n(new thread(push_thread_meta, l_istreams[l_index], opts, post_url_string, (string) *it));
		    threads.push_back(thread_n);
        }
		else 
		{
			cout << "push thread: " << post_url_string << endl;
			thread_ptr thread_n(new thread(push_thread, l_istreams[l_index], opts, post_url_string, (string) *it));
			threads.push_back(thread_n);
		}	
		l_index++;
	}

	cout << " fmp4 and CMAF ingest, press q and enter to exit " << endl;
	char c = '0';
	while ((c = cin.get()) != 'q');

	stop_all = true;

	for (auto& th : threads)
		th->join();
}
