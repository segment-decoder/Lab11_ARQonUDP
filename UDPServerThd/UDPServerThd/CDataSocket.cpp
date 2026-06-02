// CDataSocket.cpp: UDP 데이터 소켓 클래스 구현 파일입니다.
//

#include "pch.h"
#include "UDPServerThd.h"
#include "CDataSocket.h"
#include "UDPServerThdDlg.h"

CDataSocket::CDataSocket(CUDPServerThdDlg* pDlg)
{
	m_pDlg = pDlg; // 생성할 때 전달받은 대화상자 객체 주소를 저장합니다.
}

CDataSocket::~CDataSocket()
{
}
