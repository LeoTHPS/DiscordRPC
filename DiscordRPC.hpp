#pragma once
#include <AL/Common.hpp>

#include <AL/OS/System.hpp>
#include <AL/OS/Process.hpp>

#include <AL/Network/TcpSocket.hpp>
#include <AL/Network/UnixSocket.hpp>

#include <AL/Collections/Array.hpp>
#include <AL/Collections/LinkedList.hpp>

#include <json.hpp>

namespace DiscordRPC
{
	enum class UserFlags : AL::uint16
	{
		None            = 0x0000,
		Employee        = 0x0001,
		Partner         = 0x0002,
		HypeSquad       = 0x0004,
		BugHunter       = 0x0008,
		Unk0x0010       = 0x0010,
		Unk0x0020       = 0x0020,
		HouseBravery    = 0x0040,
		HouseBrilliance = 0x0080,
		HouseBalance    = 0x0100,
		EarlySupporter  = 0x0200,
		TeamUser        = 0x0400
	};

	AL_DEFINE_ENUM_FLAG_OPERATORS(UserFlags);

	enum class UserPremiumTypes : AL::uint8
	{
		None,
		NitroClassic,
		Nitro
	};

	struct User
	{
		bool                   IsBot;

		AL::String             ID;
		AL::String             Name;
		AL::BitMask<UserFlags> Flags;
		AL::String             Avatar;
		UserPremiumTypes       Premium;
		AL::String             Username;
		AL::String             Disciminator;
	};

	struct Image
	{
		AL::String Key;
		AL::String Text;
	};

	struct Button
	{
		AL::String URL;
		AL::String Label;
	};

	typedef AL::Collections::LinkedList<Button> ButtonList;

	struct RichPresence
	{
		AL::String             Header;
		AL::String             Details;
		ButtonList             Buttons;
		AL::Timestamp          TimeStart,  TimeEnd;
		Image                  ImageLarge, ImageSmall;
	};

	// @throw AL::Exception
	typedef AL::EventHandler<void(const User& user)>                                   IPCConnectionOnReadyEventHandler;
	typedef AL::EventHandler<void(AL::int32 code, const AL::String& string)>           IPCConnectionOnErrorEventHandler;

	// @throw AL::Exception
	typedef AL::EventHandler<void()>                                                   IPCConnectionOnConnectEventHandler;
	typedef AL::EventHandler<void(AL::int32 errorCode, const AL::String& errorString)> IPCConnectionOnDisconnectEventHandler;

	class IPCConnection
	{
		// https://discord.com/developers/docs/topics/rpc
		// https://discord.com/developers/docs/rich-presence/how-to
		// https://github.com/discord/discord-rpc/blob/master/documentation/hard-mode.md

		enum class OPCodes : AL::uint32
		{
			Handshake,
			Frame,
			Close,
			Ping,
			Pong
		};

		struct PacketHeader
		{
			OPCodes    OPCode;
			AL::uint32 Length;
		};

		static constexpr AL::int64 RPC_VERSION = 1;

		class PathGenerator
		{
			bool       isInitialized = false;

			AL::uint8  id;
			AL::String path;

			PathGenerator(PathGenerator&&) = delete;
			PathGenerator(const PathGenerator&) = delete;

		public:
			PathGenerator()
			{
			}

			~PathGenerator()
			{
			}

			bool IsInitialized() const
			{
				return isInitialized;
			}

			// @throw AL::Exception
			void Init()
			{
				id = 0;

#if defined(AL_PLATFORM_LINUX)
				if (!AL::OS::System::GetEnvironmentVariable(path, "XDG_RUNTIME_DIR") &&
					!AL::OS::System::GetEnvironmentVariable(path, "TMPDIR") &&
					!AL::OS::System::GetEnvironmentVariable(path, "TMP") &&
					!AL::OS::System::GetEnvironmentVariable(path, "TEMP"))
				{
					path = "/tmp"
				}

				path.Append('/');
#elif defined(AL_PLATFORM_WINDOWS)
				path = "\\\\?\\pipe\\";
#endif

				isInitialized = true;
			}

			bool Next(AL::String& value)
			{
				if (id == 10)
				{

					return false;
				}

				value = AL::String::Format(
					"%sdiscord-ipc-%u",
					path.GetCString(),
					id++
				);

				return true;
			}
		};

		bool                                             isOpen  = false;
		bool                                             isReady = false;

		User                                             user;
		AL::Network::UnixSocket<AL::Network::TcpSocket>* socket;
		AL::int32                                        errorCode;
		AL::String                                       errorString;
		AL::uint64                                       packetCounter;
		AL::String                                       applicationId;

		IPCConnection(IPCConnection&&) = delete;
		IPCConnection(const IPCConnection&) = delete;

	public:
		// @throw AL::Exception
		AL::Event<IPCConnectionOnReadyEventHandler>      OnReady;
		AL::Event<IPCConnectionOnErrorEventHandler>      OnError;

		// @throw AL::Exception
		AL::Event<IPCConnectionOnConnectEventHandler>    OnConnect;
		AL::Event<IPCConnectionOnDisconnectEventHandler> OnDisconnect;

		explicit IPCConnection(AL::String&& applicationId)
			: applicationId(
				AL::Move(applicationId)
			)
		{
		}

		virtual ~IPCConnection()
		{
			if (IsOpen())
			{

				Close();
			}
		}

		bool IsOpen() const
		{
			return isOpen;
		}

		auto& GetApplicationId() const
		{
			return applicationId;
		}

		// @throw AL::Exception
		// @return false if no connection available
		bool Open()
		{
			AL_ASSERT(
				!IsOpen(),
				"IPCConnection already open"
			);

			errorCode = 0;
			errorString.Clear();

			packetCounter = 0;

			try
			{
				if (!OpenConnection())
				{

					return false;
				}
			}
			catch (AL::Exception& exception)
			{

				throw AL::Exception(
					AL::Move(exception),
					"Error opening connection"
				);
			}

			try
			{
				if (!SendHandshake())
				{

					throw AL::Exception(
						"Connection closed"
					);
				}
			}
			catch (AL::Exception& exception)
			{
				CloseConnection();

				throw AL::Exception(
					AL::Move(exception),
					"Error sending handshake"
				);
			}

			isOpen = true;

			try
			{
				OnConnect.Execute();
			}
			catch (AL::Exception&)
			{
				Close();

				throw;
			}

			return true;
		}

		void Close()
		{
			if (IsOpen())
			{
				CloseConnection();

				isReady = false;
				isOpen  = false;

				OnDisconnect.Execute(
					errorCode,
					errorString
				);
			}
		}

		// @throw AL::Exception
		// @return false on connection closed
		bool Poll()
		{
			AL_ASSERT(
				IsOpen(),
				"IPCConnection not open"
			);

			int          value;
			PacketHeader header;

			while ((value = ReceivePacketHeader(header)) != -1)
			{
				if (value == 0)
				{
					Close();

					return false;
				}

				AL::Collections::Array<AL::uint8> buffer(
					header.Length
				);

				if (!ReceivePacketPayload(&buffer[0], header.Length))
				{
					Close();

					return false;
				}

				if (!HandlePacket(header, &buffer[0], header.Length))
				{
					Close();

					return false;
				}
			}

			return true;
		}

		// @throw AL::Exception
		// @return false on connection closed
		bool UpdatePresence(const RichPresence& value)
		{
			AL_ASSERT(
				IsOpen(),
				"IPCConnection not open"
			);

			nlohmann::json json =
			{
				{ "cmd", "SET_ACTIVITY" },
				{ "nonce", AL::ToString(packetCounter++).GetCString() }
			};

			json["args"]["pid"] = AL::OS::GetCurrentProcessId();
			json["args"]["activity"]["details"] = value.Header.GetCString();
			json["args"]["activity"]["state"]   = value.Details.GetCString();

			if (value.ImageLarge.Key.GetLength() != 0)  json["args"]["activity"]["assets"]["large_image"] = value.ImageLarge.Key.GetCString();
			if (value.ImageLarge.Text.GetLength() != 0) json["args"]["activity"]["assets"]["large_text"]  = value.ImageLarge.Text.GetCString();

			if (value.ImageSmall.Key.GetLength() != 0)  json["args"]["activity"]["assets"]["small_image"] = value.ImageSmall.Key.GetCString();
			if (value.ImageSmall.Text.GetLength() != 0) json["args"]["activity"]["assets"]["small_text"]  = value.ImageSmall.Text.GetCString();

			if (value.Buttons.GetSize())
			{
				json["args"]["activity"]["buttons"] = [&value]()
				{
					nlohmann::json::array_t buttons(
						value.Buttons.GetSize()
					);

					AL::size_t i = 0;

					for (auto& button : value.Buttons)
					{
						buttons[i++] =
						{
							{ "url", button.URL.GetCString() },
							{ "label", button.Label.GetCString() }
						};
					}

					return buttons;
				}();
			}

			if (value.TimeStart.ToSeconds() != 0) json["args"]["activity"]["timestamps"]["start"] = value.TimeStart.ToSeconds();
			if (value.TimeEnd.ToSeconds() != 0)   json["args"]["activity"]["timestamps"]["end"]   = value.TimeEnd.ToSeconds();

			if (!SendPacket(OPCodes::Frame, json))
			{

				return false;
			}

			return true;
		}

	private:
		// @throw AL::Exception
		bool OpenConnection()
		{
			PathGenerator pathGenerator;

			try
			{
				pathGenerator.Init();
			}
			catch (AL::Exception& exception)
			{

				throw AL::Exception(
					AL::Move(exception),
					"Error initializing PathGenerator"
				);
			}

			AL::String path;

			while (pathGenerator.Next(path))
			{
				socket = new AL::Network::UnixSocket<AL::Network::TcpSocket>(
					AL::Move(path)
				);

				try
				{
					if (socket->Open())
					{

						return true;
					}
				}
				catch (AL::Exception& exception)
				{
					delete socket;

					throw AL::Exception(
						AL::Move(exception),
						"Error opening AL::Network::UnixSocket<AL::Network::TcpSocket>"
					);
				}

				delete socket;
			}

			return false;
		}

		void CloseConnection()
		{
			socket->Close();
			delete socket;
		}

		// @throw AL::Exception
		// @return false on connection closed
		bool SendPacket(OPCodes opcode, const nlohmann::json& json)
		{
			auto buffer = json.dump();

			if (!SendPacket(opcode, buffer.c_str(), buffer.length()))
			{

				return false;
			}

			return true;
		}
		// @throw AL::Exception
		// @return false on connection closed
		bool SendPacket(OPCodes opcode, const void* buffer, AL::uint32 size)
		{
			PacketHeader header =
			{
				.OPCode = AL::BitConverter::ToLittleEndian(opcode),
				.Length = AL::BitConverter::ToLittleEndian(size)
			};

			for (AL::size_t numberOfBytesSent, totalBytesSent = 0; totalBytesSent < sizeof(PacketHeader); totalBytesSent += numberOfBytesSent)
			{
				if (!socket->Send(&reinterpret_cast<const AL::uint8*>(&header)[totalBytesSent], sizeof(PacketHeader) - totalBytesSent, numberOfBytesSent))
				{

					return false;
				}
			}

			for (AL::size_t numberOfBytesSent, totalBytesSent = 0; totalBytesSent < size; totalBytesSent += numberOfBytesSent)
			{
				if (!socket->Send(&reinterpret_cast<const AL::uint8*>(buffer)[totalBytesSent], size - totalBytesSent, numberOfBytesSent))
				{

					return false;
				}
			}

			return true;
		}

		// @throw AL::Exception
		// @return -1 if would block
		// @return 0 on connection closed
		int  ReceivePacketHeader(PacketHeader& value)
		{
			AL::size_t numberOfBytesReceived;

			if (!socket->Receive(&value, sizeof(PacketHeader), numberOfBytesReceived))
			{

				return 0;
			}

			if (numberOfBytesReceived == 0)
			{

				return -1;
			}

			for (AL::size_t totalBytesReceived = numberOfBytesReceived; totalBytesReceived < sizeof(PacketHeader); totalBytesReceived += numberOfBytesReceived)
			{
				if (!socket->Receive(&reinterpret_cast<AL::uint8*>(&value)[totalBytesReceived], sizeof(PacketHeader) - totalBytesReceived, numberOfBytesReceived))
				{

					return 0;
				}
			}

			value.OPCode = AL::BitConverter::FromLittleEndian(value.OPCode);
			value.Length = AL::BitConverter::FromLittleEndian(value.Length);

			return 1;
		}
		// @throw AL::Exception
		// @return false on connection closed
		bool ReceivePacketPayload(void* buffer, AL::uint32 size)
		{
			for (AL::size_t numberOfBytesReceived, totalBytesReceived = 0; totalBytesReceived < size; totalBytesReceived += numberOfBytesReceived)
			{
				if (!socket->Receive(&reinterpret_cast<AL::uint8*>(buffer)[totalBytesReceived], size - totalBytesReceived, numberOfBytesReceived))
				{

					return false;
				}
			}

			return true;
		}

	private:
		// @throw AL::Exception
		// @return false on connection closed
		bool SendHandshake()
		{
			nlohmann::json json =
			{
				{ "v", RPC_VERSION },
				{ "client_id", GetApplicationId().GetCString() }
			};

			if (!SendPacket(OPCodes::Handshake, json))
			{

				return false;
			}

			return true;
		}

	private:
		// @throw AL::Exception
		// @return false on connection closed
		bool HandlePacket(const PacketHeader& header, const void* buffer, AL::uint32 size)
		{
			switch (header.OPCode)
			{
				case OPCodes::Handshake:
					break;

				case OPCodes::Frame:
				{
					auto json = nlohmann::json::parse(
						std::string(reinterpret_cast<const char*>(buffer), header.Length)
					);

					auto it = json.find("cmd");

					if ((it != json.end()) && it->is_string())
					{
						auto cmd = it->get<std::string>();

						if (((it = json.find("evt")) != json.end()) && it->is_string())
						{
							auto evt = it->get<std::string>();

							if (!evt.compare("READY") && !cmd.compare("DISPATCH"))
							{
								if (!isReady && ((it = json.find("data")) != json.end()))
								{
									auto user_it = it->find("user");

									if (user_it != it->end())
									{
										user =
										{
											.IsBot        = (*user_it)["bot"].get<bool>(),
											.ID           = (*user_it)["id"].get<std::string>().c_str(),
											.Name         = (*user_it)["global_name"].get<std::string>().c_str(),
											.Flags        = static_cast<UserFlags>((*user_it)["flags"].get<AL::uint16>()),
											.Avatar       = (*user_it)["avatar"].get<std::string>().c_str(),
											.Premium      = static_cast<UserPremiumTypes>((*user_it)["premium_type"].get<AL::uint8>()),
											.Username     = (*user_it)["username"].get<std::string>().c_str(),
											.Disciminator = (*user_it)["discriminator"].get<std::string>().c_str()
										};

										isReady = true;

										OnReady.Execute(user);

										break;
									}
								}
							}
							else if (!evt.compare("ERROR"))
							{
								if ((it = json.find("data")) != json.end())
								{
									errorCode   = (*it)["code"].get<AL::int32>();
									errorString = (*it)["message"].get<std::string>().c_str();

									OnError.Execute(
										errorCode,
										errorString
									);
								}
							}
						}
					}
				}
				break;

				case OPCodes::Close:
				{
					auto json = nlohmann::json::parse(
						std::string(reinterpret_cast<const char*>(buffer), header.Length)
					);

					errorCode   = json["code"].get<AL::int32>();
					errorString = json["message"].get<std::string>().c_str();
				}
				return false;

				case OPCodes::Ping:
				{
					AL::Collections::Array<AL::uint8> _buffer(
						sizeof(PacketHeader) + header.Length
					);

					*reinterpret_cast<PacketHeader*>(&_buffer[0]) =
					{
						.OPCode = AL::BitConverter::ToLittleEndian(OPCodes::Pong),
						.Length = AL::BitConverter::ToLittleEndian(header.Length)
					};

					AL::memcpy(&_buffer[sizeof(PacketHeader)], buffer, header.Length);

					if (!SendPacket(OPCodes::Frame, &_buffer[0], _buffer.GetSize()))
					{

						return false;
					}
				}
				break;

				case OPCodes::Pong:
					break;
			}

			return true;
		}
	};
}
