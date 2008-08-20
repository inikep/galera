#define EVS_SEQNO_MAX 0x8000U
#include "../src/evs_seqno.hpp"
#include "../src/evs_input_map.hpp"

#include <check.h>
#include <cstdlib>

START_TEST(check_seqno)
{
    fail_unless(seqno_eq(0, 0));
    fail_unless(seqno_eq(SEQNO_MAX, SEQNO_MAX));
    fail_if(seqno_eq(5, SEQNO_MAX));
    fail_if(seqno_eq(SEQNO_MAX, 7));

    fail_unless(seqno_lt(2, 4));
    fail_unless(seqno_lt(SEQNO_MAX - 5, SEQNO_MAX - 2));
    fail_unless(seqno_lt(SEQNO_MAX - 5, 1));
    fail_if(seqno_lt(5, 5));
    fail_if(seqno_lt(SEQNO_MAX - 5, SEQNO_MAX - 5));
    fail_if(seqno_lt(5, 5 + SEQNO_MAX/2));

    fail_unless(seqno_gt(4, 2));
    fail_unless(seqno_gt(SEQNO_MAX - 2, SEQNO_MAX - 5));
    fail_unless(seqno_gt(1, SEQNO_MAX - 5));
    fail_if(seqno_gt(5, 5));
    fail_if(seqno_gt(SEQNO_MAX - 5, SEQNO_MAX - 5));
    fail_unless(seqno_gt(5, 5 + SEQNO_MAX/2));


    fail_unless(seqno_eq(seqno_add(1, 5), 6));
    fail_unless(seqno_eq(seqno_add(SEQNO_MAX - 5, 6), 1));
    fail_unless(seqno_eq(seqno_add(7, SEQNO_MAX - 5), 2));
    // fail_unless(seqno_eq(seqno_add(7, SEQNO_MAX), 7));
    // fail_unless(seqno_eq(seqno_add(SEQNO_MAX, SEQNO_MAX), 0));

    fail_unless(seqno_eq(seqno_dec(0, 1), SEQNO_MAX - 1));
    fail_unless(seqno_eq(seqno_dec(7, SEQNO_MAX - 5), 12));
    fail_unless(seqno_eq(seqno_dec(42, 5), 37));

    fail_unless(seqno_eq(seqno_next(SEQNO_MAX - 1), 0));


}
END_TEST

START_TEST(check_msg)
{
    EVSMessage umsg(EVSMessage::USER, EVSMessage::SAFE, 0x037b137bU, 0x17U, 
		    EVSViewId(Sockaddr(7), 0x7373b173U), EVSMessage::F_MSG_MORE);

    size_t buflen = umsg.size();
    uint8_t* buf = new uint8_t[buflen];


    fail_unless(umsg.write(buf, buflen, 1) == 0);
    fail_unless(umsg.write(buf, buflen, 0) == buflen);


    EVSMessage umsg2;
    fail_unless(umsg2.read(buf, buflen, 1) == 0);
    fail_unless(umsg2.read(buf, buflen, 0) == buflen);
    
    fail_unless(umsg.get_type() == umsg2.get_type());
    fail_unless(umsg.get_safety_prefix() == umsg2.get_safety_prefix());
    fail_unless(umsg.get_seq() == umsg2.get_seq());
    fail_unless(umsg.get_seq_range() == umsg2.get_seq_range());
    fail_unless(umsg.get_flags() == umsg2.get_flags());
    fail_unless(umsg.get_source_view() == umsg2.get_source_view());
}
END_TEST

START_TEST(check_input_map)
{
    EVSInputMap im;

    // Test adding and removing instances
    im.insert_sa(Sockaddr(1));
    im.insert_sa(Sockaddr(2));
    im.insert_sa(Sockaddr(3));

    try {
	im.insert_sa(Sockaddr(2));
	fail();
    } catch (FatalException e) {

    }

    im.erase_sa(Sockaddr(2));

    try {
	im.erase_sa(Sockaddr(2));
	fail();
    } catch (FatalException e) {

    }    
    im.clear();

    // Test message insert with one instance
    EVSViewId vid(Sockaddr(0), 1);
    Sockaddr sa1(1);
    im.insert_sa(sa1);
    fail_unless(seqno_eq(im.get_aru_seq(), SEQNO_MAX) && seqno_eq(im.get_safe_seq(), SEQNO_MAX));
    im.insert(sa1, 
	      EVSMessage(EVSMessage::USER, EVSMessage::SAFE, 0, 0, vid, 0),
	      0, 0);
    fail_unless(seqno_eq(im.get_aru_seq(), 0));
    
    im.insert(sa1, 
	      EVSMessage(EVSMessage::USER, EVSMessage::SAFE, 2, 0, vid, 0),
	      0, 0);
    fail_unless(seqno_eq(im.get_aru_seq(), 0));
    im.insert(sa1, 
	      EVSMessage(EVSMessage::USER, EVSMessage::SAFE, 1, 0, vid, 0),
	      0, 0);
    fail_unless(seqno_eq(im.get_aru_seq(), 2));
    
    // Must not allow insertin second instance before clear()
    try {
	im.insert_sa(Sockaddr(2));
	fail();
    } catch (FatalException e) {
	
    }
    
    im.clear();

    // Simple two instance case
    Sockaddr sa2(2);
    
    im.insert_sa(sa1);
    im.insert_sa(sa2);
 
    for (uint32_t i = 0; i < 3; i++)
	im.insert(sa1,
		  EVSMessage(EVSMessage::USER, EVSMessage::SAFE, i, 0, vid, 0),
		  0, 0);
    fail_unless(seqno_eq(im.get_aru_seq(), SEQNO_MAX));   
    
    for (uint32_t i = 0; i < 3; i++) {
	im.insert(sa2,
		  EVSMessage(EVSMessage::USER, EVSMessage::SAFE, i, 0, vid, 0),
		  0, 0);
	fail_unless(seqno_eq(im.get_aru_seq(), i));
    }

    fail_unless(seqno_eq(im.get_safe_seq(), SEQNO_MAX));

    im.set_safe(sa1, 1);
    im.set_safe(sa2, 2);
    fail_unless(seqno_eq(im.get_safe_seq(), 1));

    im.set_safe(sa1, 2);
    fail_unless(seqno_eq(im.get_safe_seq(), 2));

    
    for (EVSInputMap::iterator i = im.begin(); i != im.end(); 
	 ++i) {
	std::cerr << i.get_sockaddr().to_string() << " " << i.get_evs_message().get_seq() << "\n";
    }
    
    EVSInputMap::iterator i_next;
    for (EVSInputMap::iterator i = im.begin(); i != im.end(); 
	 i = i_next) {
	i_next = i;
	++i_next;
	std::cerr << i.get_sockaddr().to_string() << " " << i.get_evs_message().get_seq() << "\n";
	im.erase(i);
    }
    

    im.clear();

    static const size_t nodes = 16;
    static const size_t qlen = 32;
    Sockaddr sas[nodes];
    for (size_t i = 0; i < nodes; ++i) {
	sas[i] = Sockaddr(i + i);
	im.insert_sa(sas[i]);
    }
    
    Time start(Time::now());
    
    size_t n_msg = 0;
    for (uint32_t seqi = 0; seqi < 2*SEQNO_MAX; seqi++) {
	uint32_t seq = seqi % SEQNO_MAX;
	
	for (size_t j = 0; j < nodes; j++) {
	    im.insert(sas[j],
		      EVSMessage(EVSMessage::USER, EVSMessage::SAFE, seq, 0, vid, 0),
		      0, 0);	
	    n_msg++;
	}
	if (seqi > 0 && seqi % qlen == 0) {
	    uint32_t seqto = seqno_dec(seq, ::rand() % qlen + 1);
	    EVSInputMap::iterator mi_next;
	    for (EVSInputMap::iterator mi = im.begin(); mi != im.end();
		 mi = mi_next) {
		mi_next = mi;
		++mi_next;
		if (seqno_lt(mi.get_evs_message().get_seq(), seqto))
		    im.erase(mi);
		else if (seqno_eq(mi.get_evs_message().get_seq(), seqto) &&
			 ::rand() % 8 != 0)
		    im.erase(mi);
		else
		    break;
	    }
	}
    }
    EVSInputMap::iterator mi_next;
    for (EVSInputMap::iterator mi = im.begin(); mi != im.end();
	 mi = mi_next) {
	mi_next = mi;
	++mi_next;
	im.erase(mi);
    }
    Time stop(Time::now());
    std::cerr << "Msg rate " << n_msg/(stop.to_double() - start.to_double()) << "\n";
}
END_TEST


static Suite* suite()
{
    Suite* s = suite_create("evs");
    TCase* tc;

    tc = tcase_create("check_seqno");
    tcase_add_test(tc, check_seqno);
    suite_add_tcase(s, tc);

    tc = tcase_create("check_msg");
    tcase_add_test(tc, check_msg);
    suite_add_tcase(s, tc);

    tc = tcase_create("check_input_map");
    tcase_add_test(tc, check_input_map);
    suite_add_tcase(s, tc);

    return s;
}


int main()
{
    Suite* s;
    SRunner* sr;


    s = suite();
    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    int n_fail = srunner_ntests_failed(sr);
    srunner_free(sr);
    return n_fail == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
