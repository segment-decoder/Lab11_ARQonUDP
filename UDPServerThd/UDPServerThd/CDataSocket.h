// CDataSocket.h: UDP 데이터 소켓 클래스입니다.
//

#pragma once

class CUDPServerThdDlg;

class CDataSocket : public CSocket
{
public:
	CDataSocket(CUDPServerThdDlg* pDlg); // 대화상자 객체 주소를 저장하는 생성자입니다.
	virtual ~CDataSocket(); // UDP 소켓 객체를 정리하는 소멸자입니다.

	CUDPServerThdDlg* m_pDlg; // 소켓에서 대화상자 멤버에 접근하기 위한 포인터입니다.
};
