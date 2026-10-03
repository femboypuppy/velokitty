#pragma once

namespace features::world {

    class weather
    {
    public:
        void on_frame_stage_notify( );
        void on_level_shutdown( );
        void release( );

    private:
        static constexpr std::uint32_t invalid_effect_index{ static_cast<std::uint32_t>( -1 ) };

        void create_particle( );
        void update_particles( );
        void release_particles( );

        std::uint32_t m_effect_index{ invalid_effect_index };
        int m_last_particle_type{ -1 };
        float m_last_round_start_time{};
        bool m_particle_loaded{};
    };

    class scene
    {
    public:
        struct skybox_entry
        {
            std::string display_name;
            std::string resource_path;
        };

        void discover_skyboxes( );
        void reset_skybox_state( );

        void on_frame_stage_notify( );
        void on_draw_skybox_array_pre( std::uintptr_t mesh_array, int mesh_count );
        void on_draw_skybox_array_post( );
        void on_light_scene_object_pre( std::uintptr_t object ) const;
        void on_light_scene_object_post( std::uintptr_t object ) const;
        void on_draw_scene_object_array( std::uintptr_t object_array ) const;
        void on_draw_scene_object( std::uintptr_t batch, int batch_count ) const;

        enum class mesh_source : std::uint8_t
        {
            aggregate,
            animatable,
            instanced,
            count
        };

        /// World colour for the scene-object classes on_draw_scene_object never saw. Meshes that belong to a
        /// player, their gear or our viewmodel are left alone so chams and the models keep their colours.
        void on_draw_world_meshes( std::uintptr_t batch, int batch_count, mesh_source source ) const;
        [[nodiscard]] bool on_setup_fog( __m128i* output, int* mode ) const;
        void on_set_shader_param( __m128i*& value, std::uint32_t hash ) const;

       [[nodiscard]] const std::vector<skybox_entry>& get_skyboxes( ) const { return this->m_skyboxes; }

    private:
        void load_skybox_material( const char* path );

        std::vector<skybox_entry> m_skyboxes{};

        /// Resource path -> the material built for it. The material is kept alive for the process by the
        /// strong handle systems::materials::load parks in m_handles, which is what makes caching it safe
        /// across a level change; the texture it names is held by the material in turn. The resource
        /// binding the preload returned is deliberately not kept -- nothing ever needed to release it, and
        /// a stored handle nobody uses is how a "release this on map change" bug gets written later.
        std::unordered_map<std::string, std::uintptr_t> m_skybox_materials{};
        std::uintptr_t m_custom_sky_material{};
        int m_loaded_skybox_index{ -1 };

        std::uintptr_t m_active_material_binding{};
        std::uintptr_t m_active_original_material{};
        std::uintptr_t m_active_skybox_descriptor{};
        std::array<float, 3> m_active_original_sky_color{};

        /// One log line per source the first time it tints something, so a log shows which paths are live.
        mutable std::array<std::atomic<bool>, static_cast<std::size_t>( mesh_source::count )> m_source_logged{};
        bool m_active_sky_tinted{};
    };

    class smoke
    {
    public:
        void on_render_smoke_pre( ) { this->m_active = true; }
        void on_render_smoke_post( ) { this->m_active = false; }

        void on_map( std::uintptr_t token, std::size_t size, std::uintptr_t buf_ptr );
        void on_unmap( std::uintptr_t token );

        /// Writes the configured colour onto every smoke grenade entity.
        void on_frame_stage_notify( );

    private:
        float m_scale{ 255.0f };
        bool m_scale_known{};
        bool m_dumped{};

        static inline bool m_active{};
        static inline std::uintptr_t m_buf{};
        static inline std::uintptr_t m_token{};
        static inline std::size_t m_size{};
    };

} // namespace features::world
